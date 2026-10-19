#include "rcl-varlink.h"

#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <json-glib/json-glib.h>

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <string.h>
#include <sys/stat.h>

#define VARLINK_DIRECTORY "/run/systemd/userdb"
#define VARLINK_SOCKET VARLINK_DIRECTORY "/io.systemd.Home"
#define VARLINK_SERVICE "io.systemd.Home"
#define USER_DATABASE "io.systemd.UserDatabase"
#define MAX_REQUEST_SIZE (1024U * 1024U)

static GMutex nss_mutex;

typedef struct {
    gchar *user_name;
    gchar *group_name;
} Membership;

static void
membership_free(gpointer data)
{
    Membership *membership = data;

    g_free(membership->user_name);
    g_free(membership->group_name);
    g_free(membership);
}

static JsonObject *
json_object_new_empty(void)
{
    return json_object_new();
}

static void
json_object_set_string(JsonObject *object, const gchar *name,
                       const gchar *value)
{
    json_object_set_string_member(object, name, value != NULL ? value : "");
}

static void
json_object_set_number(JsonObject *object, const gchar *name, guint64 value)
{
    json_object_set_int_member(object, name, (gint64) value);
}

static gchar *
json_object_serialize(JsonObject *object)
{
    JsonNode *root = json_node_new(JSON_NODE_OBJECT);
    JsonGenerator *generator = json_generator_new();
    gchar *data;

    json_node_take_object(root, object);
    json_generator_set_root(generator, root);
    data = json_generator_to_data(generator, NULL);

    g_object_unref(generator);
    json_node_free(root);

    return data;
}

static gboolean
send_json(GOutputStream *output, JsonObject *object, GError **error)
{
    gchar *data;
    gsize length;
    const guint8 terminator = 0;
    gboolean success;

    data = json_object_serialize(object);
    length = strlen(data);

    success = g_output_stream_write_all(output, data, length, NULL, NULL,
                                        error);
    if (success)
        success = g_output_stream_write_all(output, &terminator, 1, NULL,
                                            NULL, error);

    g_free(data);
    return success;
}

static gboolean
send_reply(GOutputStream *output, JsonObject *parameters,
           gboolean continues, GError **error)
{
    JsonObject *response = json_object_new_empty();

    json_object_set_object_member(response, "parameters", parameters);
    json_object_set_boolean_member(response, "continues", continues);

    return send_json(output, response, error);
}

static gboolean
send_error(GOutputStream *output, const gchar *name, GError **error)
{
    JsonObject *response = json_object_new_empty();

    json_object_set_string_member(response, "error", name);
    json_object_set_object_member(response, "parameters",
                                  json_object_new_empty());

    return send_json(output, response, error);
}

static const gchar *
get_string(JsonObject *object, const gchar *name)
{
    JsonNode *node;

    if (!json_object_has_member(object, name))
        return NULL;

    node = json_object_get_member(object, name);
    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING)
        return NULL;

    return json_node_get_string(node);
}

static gboolean
get_number(JsonObject *object, const gchar *name, guint64 *value)
{
    JsonNode *node;
    GType type;
    gint64 number;

    if (!json_object_has_member(object, name))
        return FALSE;

    node = json_object_get_member(object, name);
    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node))
        return FALSE;

    type = json_node_get_value_type(node);
    if (type != G_TYPE_INT && type != G_TYPE_INT64 &&
        type != G_TYPE_UINT && type != G_TYPE_UINT64 &&
        type != G_TYPE_LONG && type != G_TYPE_ULONG)
        return FALSE;

    number = json_node_get_int(node);
    if (number < 0)
        return FALSE;

    *value = (guint64) number;
    return TRUE;
}

static JsonObject *
make_user_record(const struct passwd *user)
{
    JsonObject *record = json_object_new_empty();

    json_object_set_string(record, "userName", user->pw_name);
    json_object_set_number(record, "uid", (guint64) user->pw_uid);
    json_object_set_number(record, "gid", (guint64) user->pw_gid);
    json_object_set_string(record, "realName", user->pw_gecos);
    json_object_set_string(record, "homeDirectory", user->pw_dir);
    json_object_set_string(record, "shell",
        user->pw_shell != NULL && user->pw_shell[0] != '\0'
            ? user->pw_shell : "/bin/sh");
    json_object_set_string(record, "service", VARLINK_SERVICE);
    json_object_set_string(record, "disposition", "regular");

    return record;
}

static JsonObject *
make_group_record(const struct group *group)
{
    JsonObject *record = json_object_new_empty();
    JsonArray *members = json_array_new();

    json_object_set_string(record, "groupName", group->gr_name);
    json_object_set_number(record, "gid", (guint64) group->gr_gid);
    json_object_set_string(record, "service", VARLINK_SERVICE);
    json_object_set_string(record, "disposition", "regular");

    for (gchar **member = group->gr_mem; member != NULL && *member != NULL;
         member++)
        json_array_add_string_element(members, *member);

    json_object_set_array_member(record, "members", members);
    return record;
}

static gboolean
send_user_record(GOutputStream *output, const struct passwd *user,
                 GError **error)
{
    JsonObject *parameters = json_object_new_empty();

    json_object_set_object_member(parameters, "record",
                                  make_user_record(user));
    json_object_set_boolean_member(parameters, "incomplete", TRUE);

    return send_reply(output, parameters, FALSE, error);
}

static gboolean
send_group_record(GOutputStream *output, const struct group *group,
                  GError **error)
{
    JsonObject *parameters = json_object_new_empty();

    json_object_set_object_member(parameters, "record",
                                  make_group_record(group));
    json_object_set_boolean_member(parameters, "incomplete", TRUE);

    return send_reply(output, parameters, FALSE, error);
}

static gboolean
user_lookup(JsonObject *parameters, GOutputStream *output, GError **error)
{
    const gchar *name = get_string(parameters, "userName");
    guint64 uid = 0;
    gboolean has_uid = get_number(parameters, "uid", &uid);
    struct passwd *by_name = NULL;
    struct passwd *by_uid = NULL;
    struct passwd *user = NULL;

    if (name == NULL && !has_uid)
        return send_error(output, USER_DATABASE ".EnumerationNotSupported",
                          error);

    if (name != NULL)
        by_name = getpwnam(name);
    if (has_uid && uid <= (guint64) ((uid_t) -1))
        by_uid = getpwuid((uid_t) uid);

    if (name != NULL && has_uid) {
        if (by_name != NULL && by_uid != NULL &&
            by_name->pw_uid == by_uid->pw_uid)
            user = by_name;
        else if (by_name != NULL || by_uid != NULL)
            return send_error(output,
                              USER_DATABASE ".ConflictingRecordFound",
                              error);
    } else {
        user = name != NULL ? by_name : by_uid;
    }

    if (user == NULL)
        return send_error(output, USER_DATABASE ".NoRecordFound", error);

    return send_user_record(output, user, error);
}

static gboolean
group_lookup(JsonObject *parameters, GOutputStream *output, GError **error)
{
    const gchar *name = get_string(parameters, "groupName");
    guint64 gid = 0;
    gboolean has_gid = get_number(parameters, "gid", &gid);
    struct group *by_name = NULL;
    struct group *by_gid = NULL;
    struct group *group = NULL;

    if (name == NULL && !has_gid)
        return send_error(output, USER_DATABASE ".EnumerationNotSupported",
                          error);

    if (name != NULL)
        by_name = getgrnam(name);
    if (has_gid && gid <= (guint64) ((gid_t) -1))
        by_gid = getgrgid((gid_t) gid);

    if (name != NULL && has_gid) {
        if (by_name != NULL && by_gid != NULL &&
            by_name->gr_gid == by_gid->gr_gid)
            group = by_name;
        else if (by_name != NULL || by_gid != NULL)
            return send_error(output,
                              USER_DATABASE ".ConflictingRecordFound",
                              error);
    } else {
        group = name != NULL ? by_name : by_gid;
    }

    if (group == NULL)
        return send_error(output, USER_DATABASE ".NoRecordFound", error);

    return send_group_record(output, group, error);
}

static gboolean
user_is_member(const struct passwd *user, const struct group *group)
{
    if (user->pw_gid == group->gr_gid)
        return TRUE;

    for (gchar **member = group->gr_mem; member != NULL && *member != NULL;
         member++) {
        if (g_str_equal(*member, user->pw_name))
            return TRUE;
    }

    return FALSE;
}

static void
add_membership(GPtrArray *memberships, const gchar *user_name,
               const gchar *group_name)
{
    Membership *membership = g_new0(Membership, 1);

    membership->user_name = g_strdup(user_name);
    membership->group_name = g_strdup(group_name);
    g_ptr_array_add(memberships, membership);
}

static gboolean
send_memberships(GPtrArray *memberships, GOutputStream *output,
                 gboolean allow_multiple, GError **error)
{
    guint count = memberships->len;

    if (count == 0)
        return send_error(output, USER_DATABASE ".NoRecordFound", error);

    for (guint i = 0; i < count; i++) {
        Membership *membership = g_ptr_array_index(memberships, i);
        JsonObject *parameters = json_object_new_empty();
        gboolean continues = allow_multiple && i + 1 < count;

        json_object_set_string_member(parameters, "userName",
                                      membership->user_name);
        json_object_set_string_member(parameters, "groupName",
                                      membership->group_name);

        if (!send_reply(output, parameters, continues, error))
            return FALSE;

        if (!allow_multiple)
            break;
    }

    return TRUE;
}

static gboolean
get_memberships(JsonObject *parameters, GOutputStream *output,
                gboolean allow_multiple, GError **error)
{
    const gchar *user_name = get_string(parameters, "userName");
    const gchar *group_name = get_string(parameters, "groupName");
    GPtrArray *memberships = g_ptr_array_new_with_free_func(membership_free);
    struct passwd *user = NULL;
    struct group *group = NULL;
    gboolean success;

    if (user_name == NULL && group_name == NULL) {
        g_ptr_array_unref(memberships);
        return send_error(output, USER_DATABASE ".EnumerationNotSupported",
                          error);
    }

    if (user_name != NULL)
        user = getpwnam(user_name);
    if (group_name != NULL)
        group = getgrnam(group_name);

    if ((user_name != NULL && user == NULL) ||
        (group_name != NULL && group == NULL)) {
        g_ptr_array_unref(memberships);
        return send_error(output, USER_DATABASE ".NoRecordFound", error);
    }

    if (user != NULL && group != NULL) {
        if (user_is_member(user, group))
            add_membership(memberships, user->pw_name, group->gr_name);
    } else if (user != NULL) {
        int count = 16;
        gid_t *groups = g_new(gid_t, count);

        if (getgrouplist(user->pw_name, user->pw_gid, groups, &count) < 0 &&
            count > 0) {
            groups = g_renew(gid_t, groups, count);
            if (getgrouplist(user->pw_name, user->pw_gid, groups, &count) < 0)
                count = 0;
        }

        for (int i = 0; i < count; i++) {
            struct group *entry = getgrgid(groups[i]);

            if (entry != NULL)
                add_membership(memberships, user->pw_name, entry->gr_name);
        }

        g_free(groups);
    } else {
        setpwent();
        while ((user = getpwent()) != NULL) {
            if (user_is_member(user, group))
                add_membership(memberships, user->pw_name, group->gr_name);
        }
        endpwent();
    }

    success = send_memberships(memberships, output, allow_multiple, error);
    g_ptr_array_unref(memberships);
    return success;
}

static gboolean
dispatch_request(GOutputStream *output, const gchar *data, GError **error)
{
    JsonParser *parser = json_parser_new();
    JsonNode *root;
    JsonObject *request;
    JsonObject *parameters;
    const gchar *method;
    const gchar *service;
    gboolean allow_multiple = FALSE;
    gboolean success = FALSE;

    if (!json_parser_load_from_data(parser, data, -1, error)) {
        g_clear_error(error);
        success = send_error(output, "org.varlink.service.InvalidParameter",
                             error);
        goto out;
    }

    root = json_parser_get_root(parser);
    if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
        success = send_error(output, "org.varlink.service.InvalidParameter",
                             error);
        goto out;
    }

    request = json_node_get_object(root);
    method = get_string(request, "method");
    if (!json_object_has_member(request, "parameters") ||
        !JSON_NODE_HOLDS_OBJECT(json_object_get_member(request, "parameters")) ||
        method == NULL) {
        success = send_error(output, "org.varlink.service.InvalidParameter",
                             error);
        goto out;
    }

    parameters = json_object_get_object_member(request, "parameters");
    if (json_object_has_member(request, "more")) {
        JsonNode *more = json_object_get_member(request, "more");

        if (more != NULL && JSON_NODE_HOLDS_VALUE(more) &&
            json_node_get_value_type(more) == G_TYPE_BOOLEAN)
            allow_multiple = json_node_get_boolean(more);
    }

    service = get_string(parameters, "service");
    if (g_str_equal(method, USER_DATABASE ".GetUserRecord") ||
        g_str_equal(method, USER_DATABASE ".GetGroupRecord") ||
        g_str_equal(method, USER_DATABASE ".GetMemberships")) {
        if (service == NULL || !g_str_equal(service, VARLINK_SERVICE)) {
            success = send_error(output, USER_DATABASE ".BadService", error);
        } else {
            g_mutex_lock(&nss_mutex);

            if (g_str_equal(method, USER_DATABASE ".GetUserRecord"))
                success = user_lookup(parameters, output, error);
            else if (g_str_equal(method, USER_DATABASE ".GetGroupRecord"))
                success = group_lookup(parameters, output, error);
            else
                success = get_memberships(parameters, output, allow_multiple,
                                          error);

            g_mutex_unlock(&nss_mutex);
        }
    } else {
        success = send_error(output, "org.varlink.service.MethodNotFound",
                             error);
    }

out:
    g_object_unref(parser);
    return success;
}

static gpointer
serve_connection(gpointer data)
{
    GSocketConnection *connection = data;
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    GByteArray *request = g_byte_array_new();
    guint8 byte;
    GError *error = NULL;

    while (request->len < MAX_REQUEST_SIZE) {
        gssize bytes_read = g_input_stream_read(input, &byte, 1, NULL, &error);

        if (bytes_read <= 0 || byte == '\0')
            break;

        g_byte_array_append(request, &byte, 1);
    }

    if (error == NULL && request->len > 0 &&
        request->len < MAX_REQUEST_SIZE) {
        const guint8 terminator = 0;

        g_byte_array_append(request, &terminator, 1);
        dispatch_request(output, (const gchar *) request->data, &error);
    } else if (error == NULL && request->len >= MAX_REQUEST_SIZE) {
        g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE,
                            "Varlink request exceeds the size limit");
    }

    if (error != NULL) {
        g_warning("Varlink request failed: %s", error->message);
        g_error_free(error);
    }

    g_byte_array_unref(request);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);

    return NULL;
}

static gboolean
incoming_connection(GSocketService *service, GSocketConnection *connection,
                    GObject *source_object, gpointer user_data)
{
    GThread *thread;

    (void) service;
    (void) source_object;
    (void) user_data;

    thread = g_thread_new("homed-varlink", serve_connection,
                          g_object_ref(connection));
    g_thread_unref(thread);

    return TRUE;
}

GSocketService *
rcl_varlink_start(GError **error)
{
    struct stat status;
    GSocketService *service;
    GSocketAddress *address;
    GSocketAddress *effective_address = NULL;

    if (g_mkdir_with_parents(VARLINK_DIRECTORY, 0755) < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot create %s: %s", VARLINK_DIRECTORY,
                    g_strerror(errno));
        return NULL;
    }

    if (lstat(VARLINK_SOCKET, &status) == 0) {
        GSocket *probe;
        gboolean connected;

        if (!S_ISSOCK(status.st_mode)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "%s exists and is not a socket", VARLINK_SOCKET);
            return NULL;
        }

        probe = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                             G_SOCKET_PROTOCOL_DEFAULT, error);
        if (probe == NULL)
            return NULL;

        address = g_unix_socket_address_new(VARLINK_SOCKET);
        connected = g_socket_connect(probe, address, NULL, NULL);
        g_object_unref(address);
        g_object_unref(probe);

        if (connected) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_ADDRESS_IN_USE,
                        "another service is listening on %s", VARLINK_SOCKET);
            return NULL;
        }

        if (g_unlink(VARLINK_SOCKET) < 0) {
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                        "cannot remove stale socket %s: %s", VARLINK_SOCKET,
                        g_strerror(errno));
            return NULL;
        }
    } else if (errno != ENOENT) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot inspect %s: %s", VARLINK_SOCKET,
                    g_strerror(errno));
        return NULL;
    }

    service = g_socket_service_new();
    address = g_unix_socket_address_new(VARLINK_SOCKET);

    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(service), address,
            G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL,
            &effective_address, error)) {
        g_object_unref(address);
        g_object_unref(service);
        return NULL;
    }

    g_object_unref(address);
    g_clear_object(&effective_address);

    if (g_chmod(VARLINK_SOCKET, 0666) < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot set permissions on %s: %s", VARLINK_SOCKET,
                    g_strerror(errno));
        g_socket_listener_close(G_SOCKET_LISTENER(service));
        g_object_unref(service);
        g_unlink(VARLINK_SOCKET);
        return NULL;
    }

    g_signal_connect(service, "incoming", G_CALLBACK(incoming_connection),
                     NULL);
    g_socket_service_start(service);

    return service;
}

void
rcl_varlink_stop(GSocketService *service)
{
    if (service != NULL) {
        g_socket_service_stop(service);
        g_socket_listener_close(G_SOCKET_LISTENER(service));
        g_object_unref(service);
    }

    g_unlink(VARLINK_SOCKET);
}
