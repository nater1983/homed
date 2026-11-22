#include "rcl-varlink.h"
#include "config.h"
#include "rcl-nss.h"
#include "rcl-varlink-server.h"
#include "rcl-varlink-protocol.h"

#include <json-glib/json-glib.h>

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <string.h>

#define VARLINK_SERVICE "io.systemd.Home"
#define USER_DATABASE "io.systemd.UserDatabase"
#define SERVICE_INTERFACE "org.varlink.service"
#define MAX_REQUEST_SIZE (1024U * 1024U)

static const gchar user_database_description[] =
    "interface " USER_DATABASE "\n\n"
    "method GetUserRecord(uid: ?int, userName: ?string, service: string)"
    " -> (record: object, incomplete: bool)\n"
    "method GetGroupRecord(gid: ?int, groupName: ?string, service: string)"
    " -> (record: object, incomplete: bool)\n"
    "method GetMemberships(userName: ?string, groupName: ?string, service: string)"
    " -> (userName: string, groupName: string)\n"
    "error NoRecordFound()\n"
    "error BadService()\n"
    "error ServiceNotAvailable()\n"
    "error ConflictingRecordFound()\n"
    "error EnumerationNotSupported()\n";

static const gchar service_description[] =
    "interface " SERVICE_INTERFACE "\n\n"
    "method GetInfo() -> (vendor: string, product: string, version: string,"
    " url: string, interfaces: []string)\n"
    "method GetInterfaceDescription(interface: string) -> (description: string)\n"
    "error InterfaceNotFound(interface: string)\n"
    "error MethodNotFound(method: string)\n"
    "error InvalidParameter(parameter: string)\n"
    "error ExpectedMore()\n";

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
send_error_details(GOutputStream *output, const gchar *name, JsonObject *parameters,
                   GError **error)
{
    JsonObject *response = json_object_new_empty();

    json_object_set_string_member(response, "error", name);
    json_object_set_object_member(response, "parameters", parameters);

    return send_json(output, response, error);
}

static gboolean
send_error(GOutputStream *output, const gchar *name, GError **error)
{
    return send_error_details(output, name, json_object_new_empty(), error);
}

static gboolean
send_named_error(GOutputStream *output, const gchar *name, const gchar *key,
                 const gchar *value, GError **error)
{
    JsonObject *parameters = json_object_new_empty();
    json_object_set_string_member(parameters, key, value);
    return send_error_details(output, name, parameters, error);
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
                                  rcl_nss_user_record(user, VARLINK_SERVICE));
    /* This NSS record has no privileged section to redact. */
    json_object_set_boolean_member(parameters, "incomplete", FALSE);

    return send_reply(output, parameters, FALSE, error);
}

static gboolean
send_group_record(GOutputStream *output, const struct group *group,
                  GError **error)
{
    JsonObject *parameters = json_object_new_empty();

    json_object_set_object_member(parameters, "record",
                                  make_group_record(group));
    json_object_set_boolean_member(parameters, "incomplete", FALSE);

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

    uid_t name_uid = (uid_t) -1;

    if (name != NULL) {
        by_name = getpwnam(name);
        if (by_name != NULL)
            name_uid = by_name->pw_uid;
    }

    if (has_uid && uid <= (guint64) ((uid_t) -1))
        by_uid = getpwuid((uid_t) uid);

    if (name != NULL && has_uid) {
        if (by_name == NULL && by_uid == NULL)
            return send_error(output, USER_DATABASE ".NoRecordFound", error);

        if (by_name == NULL || by_uid == NULL || name_uid != by_uid->pw_uid)
            return send_error(output,
                              USER_DATABASE ".ConflictingRecordFound",
                              error);

        user = by_uid;
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

    gid_t name_gid = (gid_t) -1;

    if (name != NULL) {
        by_name = getgrnam(name);
        if (by_name != NULL)
            name_gid = by_name->gr_gid;
    }

    if (has_gid && gid <= (guint64) ((gid_t) -1))
        by_gid = getgrgid((gid_t) gid);

    if (name != NULL && has_gid) {
        if (by_name == NULL && by_gid == NULL)
            return send_error(output, USER_DATABASE ".NoRecordFound", error);

        if (by_name == NULL || by_gid == NULL || name_gid != by_gid->gr_gid)
            return send_error(output,
                              USER_DATABASE ".ConflictingRecordFound",
                              error);

        group = by_gid;
    } else {
        group = name != NULL ? by_name : by_gid;
    }

    if (group == NULL)
        return send_error(output, USER_DATABASE ".NoRecordFound", error);

    return send_group_record(output, group, error);
}

#ifdef HOMED_TESTING
static gint test_group_list_failure;

void
rcl_varlink_test_fail_group_list(gboolean enabled)
{
    g_atomic_int_set(&test_group_list_failure, enabled);
}
#endif

static int
lookup_group_list(const gchar *name, gid_t primary_gid, gid_t *groups, int *count)
{
#ifdef HOMED_TESTING
    if (g_atomic_int_get(&test_group_list_failure)) {
        errno = EIO;
        return -1;
    }
#endif
    return getgrouplist(name, primary_gid, groups, count);
}

/* Distinguish an empty membership list from an NSS lookup failure. */
static GArray *
get_user_groups(const gchar *user_name, gid_t primary_gid)
{
    int allocated = 16;
    gid_t *groups = NULL;
    GArray *result = NULL;

    /* NSS may report a larger required count on successive attempts. */
    for (guint attempt = 0; attempt < 4; attempt++) {
        int count = allocated;
        groups = g_renew(gid_t, groups, allocated);
        if (lookup_group_list(user_name, primary_gid, groups, &count) >= 0) {
            result = g_array_new(FALSE, FALSE, sizeof(gid_t));
            for (int i = 0; i < count; i++) {
                gboolean found = FALSE;
                for (guint j = 0; j < result->len; j++) {
                    if (g_array_index(result, gid_t, j) == groups[i]) {
                        found = TRUE;
                        break;
                    }
                }
                if (!found)
                    g_array_append_val(result, groups[i]);
            }
            break;
        }
        if (count <= allocated || count <= 0 || count > 65536)
            break;
        allocated = count;
    }

    g_free(groups);
    return result;
}

/* FALSE means NSS failed; membership is returned separately. */
static gboolean
user_is_member(const gchar *user_name, gid_t primary_gid, gid_t group_gid,
               gboolean *member)
{
    GArray *groups = get_user_groups(user_name, primary_gid);
    *member = FALSE;
    if (groups == NULL)
        return FALSE;

    for (guint i = 0; i < groups->len; i++) {
        if (g_array_index(groups, gid_t, i) == group_gid) {
            *member = TRUE;
            break;
        }
    }
    g_array_unref(groups);
    return TRUE;
}

typedef struct {
    gchar *name;
    gid_t primary_gid;
} UserIdentity;

static void
user_identity_free(gpointer data)
{
    UserIdentity *user = data;
    g_free(user->name);
    g_free(user);
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
                 GError **error)
{
    guint count = memberships->len;

    if (count == 0)
        return send_error(output, USER_DATABASE ".NoRecordFound", error);

    for (guint i = 0; i < count; i++) {
        Membership *membership = g_ptr_array_index(memberships, i);
        JsonObject *parameters = json_object_new_empty();
        gboolean continues = i + 1 < count;

        json_object_set_string_member(parameters, "userName",
                                      membership->user_name);
        json_object_set_string_member(parameters, "groupName",
                                      membership->group_name);

        if (!send_reply(output, parameters, continues, error))
            return FALSE;

    }

    return TRUE;
}

static gboolean
get_memberships(JsonObject *parameters, GOutputStream *output,
                gboolean allow_multiple, GError **error)
{
    const gchar *requested_user = get_string(parameters, "userName");
    const gchar *requested_group = get_string(parameters, "groupName");

    if (requested_user == NULL && requested_group == NULL)
        return send_error(output, USER_DATABASE ".EnumerationNotSupported", error);
    if (!allow_multiple && (requested_user == NULL || requested_group == NULL))
        return send_error(output, SERVICE_INTERFACE ".ExpectedMore", error);

    gchar *user_name = g_strdup(requested_user);
    gchar *group_name = g_strdup(requested_group);
    GPtrArray *memberships = g_ptr_array_new_with_free_func(membership_free);
    struct passwd *user = NULL;
    struct group *group = NULL;
    gboolean success;

    /* NSS may reuse static lookup storage. Snapshot the passwd identity
     * before looking up a group or performing another NSS operation. */
    gchar *resolved_user_name = NULL;
    gid_t resolved_primary_gid = 0;
    if (user_name != NULL) {
        user = getpwnam(user_name);
        if (user != NULL) {
            resolved_user_name = g_strdup(user->pw_name);
            resolved_primary_gid = user->pw_gid;
        }
    }
    if (group_name != NULL)
        group = getgrnam(group_name);

    if ((user_name != NULL && user == NULL) ||
        (group_name != NULL && group == NULL)) {
        g_free(group_name);
        g_free(user_name);
        g_free(resolved_user_name);
        g_ptr_array_unref(memberships);
        return send_error(output, USER_DATABASE ".NoRecordFound", error);
    }

    if (user != NULL && group != NULL) {
        const gchar *resolved_user = resolved_user_name;
        gid_t primary_gid = resolved_primary_gid;
        gid_t target_gid = group->gr_gid;
        gchar *resolved_group = g_strdup(group->gr_name);

        gboolean is_member = FALSE;
        if (!user_is_member(resolved_user, primary_gid, target_gid,
                            &is_member)) {
            g_free(resolved_group);
            goto nss_unavailable;
        }
        if (is_member)
            add_membership(memberships, resolved_user, resolved_group);

        g_free(resolved_group);
    } else if (user != NULL) {
        const gchar *resolved_user = resolved_user_name;
        gid_t primary_gid = resolved_primary_gid;
        GArray *groups = get_user_groups(resolved_user, primary_gid);

        if (groups == NULL)
            goto nss_unavailable;

        for (guint i = 0; i < groups->len; i++) {
            gid_t gid = g_array_index(groups, gid_t, i);
            struct group *entry = getgrgid(gid);
            if (entry != NULL) {
                gchar *resolved_group = g_strdup(entry->gr_name);
                add_membership(memberships, resolved_user, resolved_group);
                g_free(resolved_group);
            }
        }
        g_array_unref(groups);
    } else {
        GPtrArray *users = g_ptr_array_new_with_free_func(user_identity_free);
        gid_t target_gid = group->gr_gid;
        gchar *target_group = g_strdup(group->gr_name);

        setpwent();
        while ((user = getpwent()) != NULL) {
            UserIdentity *identity = g_new0(UserIdentity, 1);
            identity->name = g_strdup(user->pw_name);
            identity->primary_gid = user->pw_gid;
            g_ptr_array_add(users, identity);
        }
        endpwent();

        for (guint i = 0; i < users->len; i++) {
            UserIdentity *identity = g_ptr_array_index(users, i);
            gboolean is_member = FALSE;
            if (!user_is_member(identity->name, identity->primary_gid,
                                target_gid, &is_member)) {
                g_free(target_group);
                g_ptr_array_unref(users);
                goto nss_unavailable;
            }
            if (is_member)
                add_membership(memberships, identity->name, target_group);
        }
        g_free(target_group);
        g_ptr_array_unref(users);
    }

    success = send_memberships(memberships, output, error);
    g_ptr_array_unref(memberships);
    g_free(group_name);
    g_free(user_name);
    g_free(resolved_user_name);
    return success;

nss_unavailable:
    g_ptr_array_unref(memberships);
    g_free(group_name);
    g_free(user_name);
    g_free(resolved_user_name);
    return send_error(output, USER_DATABASE ".ServiceNotAvailable", error);
}

static gboolean
send_service_info(GOutputStream *output, GError **error)
{
    JsonObject *parameters = json_object_new_empty();
    JsonArray *interfaces = json_array_new();

    json_object_set_string_member(parameters, "vendor", VARLINK_VENDOR);
    json_object_set_string_member(parameters, "product", "homed");
    json_object_set_string_member(parameters, "version", PACKAGE_VERSION);
    json_object_set_string_member(parameters, "url",
                                  "https://github.com/nater1983/homed");
    json_array_add_string_element(interfaces, SERVICE_INTERFACE);
    json_array_add_string_element(interfaces, USER_DATABASE);
    json_object_set_array_member(parameters, "interfaces", interfaces);

    return send_reply(output, parameters, FALSE, error);
}

static gboolean
send_interface_description(GOutputStream *output, JsonObject *request, GError **error)
{
    const gchar *interface = get_string(request, "interface");
    const gchar *description;

    if (interface == NULL)
        return send_named_error(output, SERVICE_INTERFACE ".InvalidParameter",
                                "parameter", "interface", error);
    if (g_str_equal(interface, USER_DATABASE))
        description = user_database_description;
    else if (g_str_equal(interface, SERVICE_INTERFACE))
        description = service_description;
    else
        return send_named_error(output, SERVICE_INTERFACE ".InterfaceNotFound",
                                "interface", interface, error);

    JsonObject *parameters = json_object_new_empty();
    json_object_set_string_member(parameters, "description", description);
    return send_reply(output, parameters, FALSE, error);
}

static const gchar *
invalid_lookup_parameter(JsonObject *parameters, const gchar *id_key,
                         const gchar *first_name_key, const gchar *second_name_key)
{
    JsonObjectIter iter;
    const gchar *name;
    JsonNode *value;

    json_object_iter_init(&iter, parameters);
    while (json_object_iter_next(&iter, &name, &value)) {
        if (g_str_equal(name, "service"))
            continue;
        if (g_strcmp0(name, id_key) == 0) {
            guint64 id;
            if (JSON_NODE_HOLDS_NULL(value) ||
                (get_number(parameters, name, &id) && id < G_MAXUINT32))
                continue;
        } else if (g_str_equal(name, first_name_key) ||
                   g_strcmp0(name, second_name_key) == 0) {
            if (JSON_NODE_HOLDS_NULL(value) || get_string(parameters, name) != NULL)
                continue;
        }
        return name;
    }
    return NULL;
}

#ifdef HOMED_TESTING
/* Test-only pause point: NSS has been unlocked, but the client reply
 * has not yet been written. */
static void (*before_nss_reply_write)(void);

void
rcl_varlink_test_set_before_nss_reply_write(void (*callback)(void))
{
    before_nss_reply_write = callback;
}
#endif

static gboolean
dispatch_request(GOutputStream *output, const gchar *data, GError **error)
{
    JsonParser *parser = json_parser_new();
    JsonNode *root;
    JsonObject *request;
    JsonObject *parameters;
    const gchar *method;
    const gchar *service;
    g_autoptr(JsonObject) empty_parameters = NULL;
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
    JsonNode *parameters_node = json_object_get_member(request, "parameters");
    if (method == NULL || (parameters_node != NULL &&
                           !JSON_NODE_HOLDS_OBJECT(parameters_node))) {
        success = send_error(output, "org.varlink.service.InvalidParameter",
                             error);
        goto out;
    }

    if (parameters_node == NULL)
        empty_parameters = json_object_new_empty();
    parameters = parameters_node != NULL ? json_node_get_object(parameters_node)
                                         : empty_parameters;
    if (json_object_has_member(request, "more")) {
        JsonNode *more = json_object_get_member(request, "more");

        if (more == NULL || !JSON_NODE_HOLDS_VALUE(more) ||
            json_node_get_value_type(more) != G_TYPE_BOOLEAN) {
            success = send_error(output, "org.varlink.service.InvalidParameter",
                                 error);
            goto out;
        }
        allow_multiple = json_node_get_boolean(more);
    }

    if (g_str_equal(method, SERVICE_INTERFACE ".GetInfo")) {
        success = send_service_info(output, error);
        goto out;
    }
    if (g_str_equal(method, SERVICE_INTERFACE ".GetInterfaceDescription")) {
        success = send_interface_description(output, parameters, error);
        goto out;
    }

    service = get_string(parameters, "service");
    gboolean user_record = g_str_equal(method, USER_DATABASE ".GetUserRecord");
    gboolean group_record = g_str_equal(method, USER_DATABASE ".GetGroupRecord");
    if (user_record || group_record ||
        g_str_equal(method, USER_DATABASE ".GetMemberships")) {
        if (g_strcmp0(service, VARLINK_SERVICE) != 0) {
            success = send_error(output, USER_DATABASE ".BadService", error);
            goto out;
        }
        const gchar *invalid = invalid_lookup_parameter(parameters,
            user_record ? "uid" : group_record ? "gid" : NULL,
            group_record ? "groupName" : "userName",
            user_record || group_record ? NULL : "groupName");
        if (invalid != NULL) {
            success = send_named_error(output, SERVICE_INTERFACE ".InvalidParameter",
                                       "parameter", invalid, error);
        } else if (!rcl_nss_lock_timeout(1000)) {
            success = send_error(output, USER_DATABASE ".ServiceNotAvailable",
                                 error);
        } else {
            /*
             * Resolve and serialize NSS-backed records while holding the NSS
             * mutex, but never write to a client socket under that mutex.
             * A client that stops reading must not block other lookups.
             */
            GOutputStream *buffer = g_memory_output_stream_new_resizable();
            GError *buffer_error = NULL;
            gboolean prepared;

            if (user_record)
                prepared = user_lookup(parameters, buffer, &buffer_error);
            else if (group_record)
                prepared = group_lookup(parameters, buffer, &buffer_error);
            else
                prepared = get_memberships(parameters, buffer, allow_multiple,
                                           &buffer_error);

            rcl_nss_unlock();

            if (prepared) {
#ifdef HOMED_TESTING
                if (before_nss_reply_write != NULL)
                    before_nss_reply_write();
#endif
                gsize length = g_memory_output_stream_get_data_size(
                    G_MEMORY_OUTPUT_STREAM(buffer));
                gpointer bytes = g_memory_output_stream_get_data(
                    G_MEMORY_OUTPUT_STREAM(buffer));
                success = g_output_stream_write_all(output, bytes, length,
                                                    NULL, NULL, error);
            } else {
                success = FALSE;
                if (buffer_error != NULL)
                    g_propagate_error(error, buffer_error);
                else
                    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                                        "Failed to prepare Varlink response");
            }
            g_object_unref(buffer);
        }
    } else {
        success = send_named_error(output, SERVICE_INTERFACE ".MethodNotFound",
                                   "method", method, error);
    }

out:
    g_object_unref(parser);
    return success;
}

void
rcl_varlink_serve_connection(GSocketConnection *connection)
{
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    GByteArray *request = g_byte_array_new();
    guint8 byte;
    GError *error = NULL;

    for (;;) {
        gssize bytes_read = g_input_stream_read(input, &byte, 1, NULL, &error);
        if (bytes_read <= 0)
            break;
        if (byte == '\0') {
            const guint8 terminator = 0;
            if (request->len == 0) {
                if (!send_error(output, "org.varlink.service.InvalidParameter",
                                &error))
                    break;
                continue;
            }
            g_byte_array_append(request, &terminator, 1);
            if (!dispatch_request(output, (const gchar *) request->data,
                                  &error) || error != NULL)
                break;
            g_byte_array_set_size(request, 0);
            continue;
        }
        if (request->len >= MAX_REQUEST_SIZE) {
            send_error(output, "org.varlink.service.InvalidParameter", &error);
            break;
        }
        g_byte_array_append(request, &byte, 1);
    }
    if (error != NULL) {
        /* Peer disconnects and shutdown-induced socket errors are expected
         * while handling slow or abandoned clients. */
        if (!g_error_matches(error, G_IO_ERROR, G_IO_ERROR_BROKEN_PIPE) &&
            !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED) &&
            !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
            !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT))
            g_warning("Varlink request failed: %s", error->message);
        g_error_free(error);
    }
    g_byte_array_unref(request);
}

GSocketService *
rcl_varlink_start_at(const gchar *directory, const gchar *socket_path,
                     GError **error)
{
    return rcl_varlink_server_start_at(directory, socket_path, error);
}

GSocketService *
rcl_varlink_start(GError **error)
{
    return rcl_varlink_server_start("/run", error);
}

void
rcl_varlink_stop(GSocketService *service)
{
    rcl_varlink_server_stop(service);
}
