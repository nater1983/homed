#include "rcl-home.h"
#include "rcl-varlink.h"
#include "rcl-nss.h"
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>

#include <gio/gio.h>
#include <unistd.h>

#define BUS_NAME "org.freedesktop.home1"
#define OBJECT_PATH "/org/freedesktop/home1"
#define MANAGER_IFACE "org.freedesktop.home1.Manager"

typedef struct {
    GTestDBus *bus;
    GThread *thread;
    GMainContext *context;
    GMainLoop *loop;
    GDBusConnection *client;
    GMutex mutex;
    GCond cond;
    gboolean ready;
    GError *startup_error;
    GMainContext *varlink_context;
    GMainLoop *varlink_loop;
    GThread *varlink_thread;
    GMutex varlink_mutex;
    GCond varlink_cond;
    gboolean varlink_ready;
    GError *varlink_error;
    GSocketService *varlink_service;
    gchar *varlink_directory;
    gchar *varlink_socket;
} Fixture;

static gpointer
service_thread(gpointer data)
{
    Fixture *fixture = data;
    GError *error = NULL;
    GDBusConnection *connection;
    RclHome *home;
    guint reply = 0;

    fixture->context = g_main_context_new();
    g_main_context_push_thread_default(fixture->context);
    connection = g_dbus_connection_new_for_address_sync(
        g_test_dbus_get_bus_address(fixture->bus),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &error);

    if (connection != NULL) {
        GVariant *name_reply = g_dbus_connection_call_sync(
            connection, "org.freedesktop.DBus", "/org/freedesktop/DBus",
            "org.freedesktop.DBus", "RequestName",
            g_variant_new("(su)", BUS_NAME, 0u), G_VARIANT_TYPE("(u)"),
            G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
        if (name_reply != NULL) {
            g_variant_get(name_reply, "(u)", &reply);
            g_variant_unref(name_reply);
        }
    }

    home = connection != NULL && error == NULL ? rcl_home_new() : NULL;
    if (home == NULL && error == NULL)
        g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "cannot initialize home1 test service");
    if (home != NULL && reply != 1 && reply != 4)
        g_set_error(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "cannot own D-Bus test name (reply %u)", reply);
    if (home != NULL && error == NULL &&
        !rcl_home_register(home, connection, &error) && error == NULL)
        g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "cannot register home1 test objects");

    g_mutex_lock(&fixture->mutex);
    fixture->loop = error == NULL ? g_main_loop_new(fixture->context, FALSE) : NULL;
    fixture->startup_error = error;
    fixture->ready = TRUE;
    g_cond_signal(&fixture->cond);
    g_mutex_unlock(&fixture->mutex);

    if (error == NULL) {
        g_main_loop_run(fixture->loop);
        g_main_loop_unref(fixture->loop);
        fixture->loop = NULL;
    }

    if (home != NULL)
        rcl_home_free(home);
    if (connection != NULL) {
        g_dbus_connection_close_sync(connection, NULL, NULL);
        g_object_unref(connection);
    }
    g_main_context_pop_thread_default(fixture->context);
    g_main_context_unref(fixture->context);
    return NULL;
}

static gboolean
quit_service(gpointer data)
{
    Fixture *fixture = data;
    if (fixture->loop != NULL)
        g_main_loop_quit(fixture->loop);
    return G_SOURCE_REMOVE;
}


static gpointer
varlink_service_thread(gpointer data)
{
    Fixture *fixture = data;
    GError *error = NULL;

    fixture->varlink_context = g_main_context_new();
    g_main_context_push_thread_default(fixture->varlink_context);
    fixture->varlink_directory = g_dir_make_tmp("homed-mixed-test-XXXXXX", &error);
    if (fixture->varlink_directory != NULL) {
        fixture->varlink_socket = g_build_filename(fixture->varlink_directory,
                                                    "io.systemd.Home", NULL);
        fixture->varlink_service = rcl_varlink_start_at(
            fixture->varlink_directory, fixture->varlink_socket, &error);
    }

    if (fixture->varlink_service != NULL)
        fixture->varlink_loop = g_main_loop_new(fixture->varlink_context, FALSE);

    g_mutex_lock(&fixture->varlink_mutex);
    fixture->varlink_error = error;
    fixture->varlink_ready = TRUE;
    g_cond_signal(&fixture->varlink_cond);
    g_mutex_unlock(&fixture->varlink_mutex);

    if (fixture->varlink_loop != NULL) {
        g_main_loop_run(fixture->varlink_loop);
        rcl_varlink_stop(fixture->varlink_service);
        fixture->varlink_service = NULL;
        /* Closing GSocketListener cancels its outstanding async accept.
         * Dispatch that completion before destroying the owning context. */
        while (g_main_context_iteration(fixture->varlink_context, FALSE))
            ;
        g_main_loop_unref(fixture->varlink_loop);
        fixture->varlink_loop = NULL;
    }

    if (fixture->varlink_directory != NULL)
        g_rmdir(fixture->varlink_directory);
    g_main_context_pop_thread_default(fixture->varlink_context);
    g_main_context_unref(fixture->varlink_context);
    return NULL;
}

static gboolean
quit_varlink(gpointer data)
{
    Fixture *fixture = data;
    if (fixture->varlink_loop != NULL)
        g_main_loop_quit(fixture->varlink_loop);
    return G_SOURCE_REMOVE;
}

static void
setup(Fixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    (void) data;

    g_mutex_init(&fixture->mutex);
    g_cond_init(&fixture->cond);
    g_mutex_init(&fixture->varlink_mutex);
    g_cond_init(&fixture->varlink_cond);

    /* GTestDBus forks a watcher on first use. Start it before either fixture
     * thread so it cannot inherit their live allocations. */
    fixture->bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(fixture->bus);

    fixture->varlink_thread = g_thread_new("homed-varlink-test",
        varlink_service_thread, fixture);
    g_mutex_lock(&fixture->varlink_mutex);
    while (!fixture->varlink_ready)
        g_cond_wait(&fixture->varlink_cond, &fixture->varlink_mutex);
    g_mutex_unlock(&fixture->varlink_mutex);
    g_assert_no_error(fixture->varlink_error);
    fixture->thread = g_thread_new("homed-dbus-test",
                                   service_thread, fixture);

    g_mutex_lock(&fixture->mutex);
    while (!fixture->ready)
        g_cond_wait(&fixture->cond, &fixture->mutex);
    g_mutex_unlock(&fixture->mutex);
    g_assert_no_error(fixture->startup_error);

    fixture->client = g_dbus_connection_new_for_address_sync(
        g_test_dbus_get_bus_address(fixture->bus),
        G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
        G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(fixture->client);
}

static void
teardown(Fixture *fixture, gconstpointer data)
{
    (void) data;
    g_dbus_connection_close_sync(fixture->client, NULL, NULL);
    g_clear_object(&fixture->client);
    g_main_context_invoke(fixture->context, quit_service, fixture);
    g_thread_join(fixture->thread);
    g_main_context_invoke(fixture->varlink_context, quit_varlink, fixture);
    g_thread_join(fixture->varlink_thread);
    g_test_dbus_down(fixture->bus);
    g_object_unref(fixture->bus);
    g_clear_error(&fixture->startup_error);
    g_free(fixture->varlink_directory);
    g_free(fixture->varlink_socket);
    g_clear_error(&fixture->varlink_error);
    g_cond_clear(&fixture->varlink_cond);
    g_mutex_clear(&fixture->varlink_mutex);
    g_cond_clear(&fixture->cond);
    g_mutex_clear(&fixture->mutex);
}

static GVariant *
call_manager(Fixture *fixture, const gchar *method, GVariant *parameters,
             const GVariantType *reply_type, GError **error)
{
    return g_dbus_connection_call_sync(fixture->client, BUS_NAME, OBJECT_PATH,
        MANAGER_IFACE, method, parameters, reply_type,
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, error);
}

static void
assert_user_record_json(const gchar *json)
{
    g_autoptr(JsonParser) parser = json_parser_new();
    g_autoptr(GError) error = NULL;

    g_assert_true(json_parser_load_from_data(parser, json, -1, &error));
    g_assert_no_error(error);
    JsonNode *root = json_parser_get_root(parser);
    g_assert_true(JSON_NODE_HOLDS_OBJECT(root));
    JsonObject *record = json_node_get_object(root);
    g_assert_cmpstr(json_object_get_string_member(record, "userName"), ==, "root");
    g_assert_cmpint(json_object_get_int_member(record, "uid"), ==, 0);
    g_assert_cmpstr(json_object_get_string_member(record, "service"), ==, "nss");
    g_assert_false(json_object_has_member(record, "secret"));
    g_assert_false(json_object_has_member(record, "privileged"));
}

static GVariant *
read_home_property(Fixture *fixture, const gchar *path, const gchar *name)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) reply = g_dbus_connection_call_sync(
        fixture->client, BUS_NAME, path, "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.freedesktop.home1.Home", name),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    g_autoptr(GVariant) boxed = g_variant_get_child_value(reply, 0);
    return g_variant_get_variant(boxed);
}

static void
assert_user_record(Fixture *fixture, GVariant *reply)
{
    const gchar *json, *path, *property_json;
    const gchar *name, *gecos, *directory, *shell;
    guint32 uid, gid;
    gboolean incomplete;

    g_variant_get(reply, "(&sb&o)", &json, &incomplete, &path);
    g_assert_false(incomplete);
    assert_user_record_json(json);

    g_autoptr(GVariant) unix_record = read_home_property(fixture, path, "UnixRecord");
    g_assert_true(g_variant_is_of_type(unix_record, G_VARIANT_TYPE("(suusss)")));
    g_variant_get(unix_record, "(&suu&s&s&s)", &name, &uid, &gid,
                  &gecos, &directory, &shell);
    g_assert_cmpstr(name, ==, "root");
    g_assert_cmpuint(uid, ==, 0);
    g_assert_cmpstr(directory, !=, "");
    g_assert_cmpstr(shell, !=, "");

    g_autoptr(GVariant) record = read_home_property(fixture, path, "UserRecord");
    g_assert_true(g_variant_is_of_type(record, G_VARIANT_TYPE("(sb)")));
    g_variant_get(record, "(&sb)", &property_json, &incomplete);
    g_assert_false(incomplete);
    g_assert_cmpstr(property_json, ==, json);
}

static void
test_nss_lock_contention(Fixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    GVariant *reply;
    (void) data;

    rcl_nss_lock();
    reply = call_manager(fixture, "GetUserRecordByName",
        g_variant_new("(s)", "root"), G_VARIANT_TYPE("(sbo)"), &error);
    rcl_nss_unlock();

    g_assert_null(reply);
    g_assert_nonnull(error);
    g_assert_true(g_dbus_error_is_remote_error(error));
    g_clear_error(&error);
}

static void
test_record_by_name(Fixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    GVariant *reply = call_manager(fixture, "GetUserRecordByName",
        g_variant_new("(s)", "root"), G_VARIANT_TYPE("(sbo)"), &error);
    (void) data;
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    assert_user_record(fixture, reply);
    g_variant_unref(reply);
}

static void
test_record_by_uid(Fixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    GVariant *reply = call_manager(fixture, "GetUserRecordByUID",
        g_variant_new("(u)", 0u), G_VARIANT_TYPE("(sbo)"), &error);
    (void) data;
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    assert_user_record(fixture, reply);
    g_variant_unref(reply);
}

static void
test_record_by_caller(Fixture *fixture, gconstpointer data)
{
    g_autoptr(GError) error = NULL;
    (void) data;
    g_autoptr(GVariant) expected = call_manager(fixture, "GetUserRecordByUID",
        g_variant_new("(u)", (guint32) geteuid()), G_VARIANT_TYPE("(sbo)"), &error);
    g_assert_no_error(error);
    g_assert_nonnull(expected);
    g_autoptr(GVariant) actual = call_manager(fixture, "GetUserRecordByName",
        g_variant_new("(s)", ""), G_VARIANT_TYPE("(sbo)"), &error);
    g_assert_no_error(error);
    g_assert_nonnull(actual);
    g_assert_true(g_variant_equal(expected, actual));

    g_autoptr(GVariant) record_path = g_variant_get_child_value(actual, 2);
    g_autoptr(GVariant) home = call_manager(fixture, "GetHomeByName",
        g_variant_new("(s)", ""), G_VARIANT_TYPE("(usussso)"), &error);
    g_assert_no_error(error);
    g_assert_nonnull(home);
    g_autoptr(GVariant) home_path = g_variant_get_child_value(home, 6);
    g_assert_true(g_variant_equal(record_path, home_path));
}

static void
test_record_by_pid(Fixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    GVariant *reply = call_manager(fixture, "GetUserRecordByPID",
        g_variant_new("(u)", (guint32) getpid()),
        G_VARIANT_TYPE("(a{sv}b)"), &error);
    (void) data;
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    g_assert_true(g_variant_is_of_type(reply, G_VARIANT_TYPE("(a{sv}b)")));
    g_variant_unref(reply);
}

/* Reject invalid unsigned PIDs without resolving them to NSS records. */
static void
test_invalid_pids(Fixture *fixture, gconstpointer data)
{
    const guint32 invalid[] = { 0u, G_MAXINT32 + 1u, G_MAXUINT32 };
    (void) data;
    for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
        g_autoptr(GError) error = NULL;
        g_autoptr(GVariant) reply = call_manager(fixture, "GetUserRecordByPID",
            g_variant_new("(u)", invalid[i]), G_VARIANT_TYPE("(a{sv}b)"),
            &error);
        g_assert_null(reply);
        g_assert_nonnull(error);
        g_assert_true(g_dbus_error_is_remote_error(error));
        g_autofree gchar *name = g_dbus_error_get_remote_error(error);
        g_assert_cmpstr(name, ==, "org.freedesktop.home1.NoSuchUser");
    }
}

static void
test_missing_user(Fixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    GVariant *reply = call_manager(fixture, "GetUserRecordByName",
        g_variant_new("(s)", "homed-test-user-that-does-not-exist"),
        G_VARIANT_TYPE("(sbo)"), &error);
    (void) data;
    g_assert_null(reply);
    g_assert_nonnull(error);
    g_assert_true(g_dbus_error_is_remote_error(error));
    g_autofree gchar *name = g_dbus_error_get_remote_error(error);
    g_assert_cmpstr(name, ==, "org.freedesktop.home1.NoSuchHome");
    g_clear_error(&error);
}

static void
test_list_homes(Fixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    GVariant *reply = call_manager(fixture, "ListHomes", NULL,
        G_VARIANT_TYPE("(a(susussso))"), &error);
    GVariant *homes;
    (void) data;
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    g_variant_get(reply, "(@a(susussso))", &homes);
    g_assert_cmpuint(g_variant_n_children(homes), >, 0);
    g_variant_unref(homes);
    g_variant_unref(reply);
}

static void
test_home_by_name(Fixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    GVariant *reply = call_manager(fixture, "GetHomeByName",
        g_variant_new("(s)", "root"), G_VARIANT_TYPE("(usussso)"), &error);
    GVariant *path_value;
    const gchar *path;
    (void) data;
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    path_value = g_variant_get_child_value(reply, 6);
    path = g_variant_get_string(path_value, NULL);
    g_assert_true(g_str_has_prefix(path, "/org/freedesktop/home1/home/_"));
    g_variant_unref(path_value);
    g_variant_unref(reply);
}


static void
test_home_by_uid(Fixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    GVariant *reply = call_manager(fixture, "GetHomeByUID",
        g_variant_new("(u)", 0u), G_VARIANT_TYPE("(ssussso)"), &error);
    GVariant *name_value;
    (void) data;

    g_assert_no_error(error);
    g_assert_nonnull(reply);
    name_value = g_variant_get_child_value(reply, 0);
    g_assert_cmpstr(g_variant_get_string(name_value, NULL), ==, "root");
    g_variant_unref(name_value);
    g_variant_unref(reply);
}

static void
test_home_property(Fixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    GVariant *home = call_manager(fixture, "GetHomeByName",
        g_variant_new("(s)", "root"), G_VARIANT_TYPE("(usussso)"), &error);
    GVariant *path_value, *state_value;
    const gchar *path, *manager_state;
    GVariant *reply, *boxed, *value;
    (void) data;
    g_assert_no_error(error);
    g_assert_nonnull(home);
    state_value = g_variant_get_child_value(home, 1);
    manager_state = g_variant_get_string(state_value, NULL);
    path_value = g_variant_get_child_value(home, 6);
    path = g_variant_get_string(path_value, NULL);
    reply = g_dbus_connection_call_sync(fixture->client, BUS_NAME, path,
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.freedesktop.home1.Home", "UserName"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    g_variant_get(reply, "(@v)", &boxed);
    value = g_variant_get_variant(boxed);
    g_assert_cmpstr(g_variant_get_string(value, NULL), ==, "root");
    g_variant_unref(value);
    g_variant_unref(boxed);
    g_variant_unref(reply);

    reply = g_dbus_connection_call_sync(fixture->client, BUS_NAME, path,
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.freedesktop.home1.Home", "State"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    g_variant_get(reply, "(@v)", &boxed);
    value = g_variant_get_variant(boxed);
    g_assert_cmpstr(g_variant_get_string(value, NULL), ==, manager_state);
    g_variant_unref(value);
    g_variant_unref(boxed);
    g_variant_unref(reply);
    g_variant_unref(state_value);
    g_variant_unref(path_value);
    g_variant_unref(home);
}



static void
test_unsupported_home_operation(Fixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    GVariant *home = call_manager(fixture, "GetHomeByName",
        g_variant_new("(s)", "root"), G_VARIANT_TYPE("(usussso)"), &error);
    GVariant *path_value;
    const gchar *path;
    GVariant *reply;
    gchar *remote_name;
    (void) data;

    g_assert_no_error(error);
    g_assert_nonnull(home);
    path_value = g_variant_get_child_value(home, 6);
    path = g_variant_get_string(path_value, NULL);
    reply = g_dbus_connection_call_sync(fixture->client, BUS_NAME, path,
        "org.freedesktop.home1.Home", "Activate", NULL, NULL,
        G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    g_assert_null(reply);
    g_assert_nonnull(error);
    remote_name = g_dbus_error_get_remote_error(error);
    g_assert_cmpstr(remote_name, ==, "org.freedesktop.home1.NotSupported");
    g_free(remote_name);
    g_clear_error(&error);
    g_variant_unref(path_value);
    g_variant_unref(home);
}

static void
test_read_only_home_property(Fixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    GVariant *home = call_manager(fixture, "GetHomeByName",
        g_variant_new("(s)", "root"), G_VARIANT_TYPE("(usussso)"), &error);
    GVariant *path_value;
    const gchar *path;
    GVariant *reply;
    GVariant *boxed, *value;
    (void) data;

    g_assert_no_error(error);
    g_assert_nonnull(home);
    path_value = g_variant_get_child_value(home, 6);
    path = g_variant_get_string(path_value, NULL);
    reply = g_dbus_connection_call_sync(fixture->client, BUS_NAME, path,
        "org.freedesktop.DBus.Properties", "Set",
        g_variant_new("(ssv)", "org.freedesktop.home1.Home", "UserName",
                      g_variant_new_string("changed")),
        NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    g_assert_null(reply);
    g_assert_nonnull(error);
    g_assert_true(g_dbus_error_is_remote_error(error));
    g_clear_error(&error);

    reply = g_dbus_connection_call_sync(fixture->client, BUS_NAME, path,
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", "org.freedesktop.home1.Home", "UserName"),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    g_variant_get(reply, "(@v)", &boxed);
    value = g_variant_get_variant(boxed);
    g_assert_cmpstr(g_variant_get_string(value, NULL), ==, "root");
    g_variant_unref(value);
    g_variant_unref(boxed);
    g_variant_unref(reply);
    g_variant_unref(path_value);
    g_variant_unref(home);
}

static void
test_unsupported_home_operations(Fixture *fixture, gconstpointer data)
{
    static const struct {
        const gchar *name;
        GVariant *(*parameters)(void);
    } operations[] = {
        { "Activate", NULL },
        { "Deactivate", NULL },
        { "Lock", NULL },
        { "Unlock", NULL },
        { "Resize", NULL }
    };
    GError *error = NULL;
    GVariant *home = call_manager(fixture, "GetHomeByName",
        g_variant_new("(s)", "root"), G_VARIANT_TYPE("(usussso)"), &error);
    GVariant *path_value;
    const gchar *path;
    (void) data;

    g_assert_no_error(error);
    g_assert_nonnull(home);
    path_value = g_variant_get_child_value(home, 6);
    path = g_variant_get_string(path_value, NULL);

    for (guint i = 0; i < G_N_ELEMENTS(operations); i++) {
        GVariant *reply;
        gchar *remote_name;

        GVariant *parameters = g_str_equal(operations[i].name, "Resize")
            ? g_variant_new("(t)", (guint64) 0) : NULL;
        reply = g_dbus_connection_call_sync(fixture->client, BUS_NAME, path,
            "org.freedesktop.home1.Home", operations[i].name, parameters,
            NULL, G_DBUS_CALL_FLAGS_NONE, 5000, NULL, &error);
        g_assert_null(reply);
        g_assert_nonnull(error);
        remote_name = g_dbus_error_get_remote_error(error);
        g_assert_cmpstr(remote_name, ==, "org.freedesktop.home1.NotSupported");
        g_free(remote_name);
        g_clear_error(&error);
    }

    g_variant_unref(path_value);
    g_variant_unref(home);
}

static gpointer
mixed_nss_worker(gpointer data)
{
    Fixture *fixture = data;

    for (guint i = 0; i < 20; i++) {
        GError *error = NULL;
        GVariant *reply = call_manager(fixture, "GetUserRecordByName",
            g_variant_new("(s)", "root"), G_VARIANT_TYPE("(sbo)"), &error);
        if (reply == NULL || error != NULL) {
            g_clear_error(&error);
            g_clear_pointer(&reply, g_variant_unref);
            return GINT_TO_POINTER(1);
        }
        g_variant_unref(reply);

        GSocketClient *client = g_socket_client_new();
        GSocketAddress *address =
            g_unix_socket_address_new(fixture->varlink_socket);
        GSocketConnection *connection =
            g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address),
                                    NULL, &error);
        if (connection == NULL) {
            g_clear_error(&error);
            g_object_unref(address);
            g_object_unref(client);
            return GINT_TO_POINTER(1);
        }

        const gchar request[] =
            "{\"method\":\"io.systemd.UserDatabase.GetUserRecord\","
            "\"parameters\":{\"service\":\"io.systemd.Home\","
            "\"userName\":\"root\"}}";
        gchar *framed = g_strconcat(request, "\0", NULL);
        GOutputStream *output =
            g_io_stream_get_output_stream(G_IO_STREAM(connection));
        GInputStream *input =
            g_io_stream_get_input_stream(G_IO_STREAM(connection));
        gboolean ok = g_output_stream_write_all(output, framed,
            strlen(framed) + 1, NULL, NULL, &error);
        g_free(framed);

        gchar response[4096];
        gsize used = 0;
        gboolean terminated = FALSE;
        while (ok && used < sizeof(response) - 1) {
            guint8 byte;
            gssize count = g_input_stream_read(input, &byte, 1, NULL, &error);
            if (count != 1) {
                ok = FALSE;
                break;
            }
            if (byte == 0) {
                terminated = TRUE;
                break;
            }
            response[used++] = (gchar) byte;
        }
        response[used] = '\0';
        ok = ok && terminated && error == NULL &&
             strstr(response, "\"userName\":\"root\"") != NULL;
        g_clear_error(&error);
        g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
        g_object_unref(connection);
        g_object_unref(address);
        g_object_unref(client);
        if (!ok)
            return GINT_TO_POINTER(1);
    }

    return NULL;
}

static void
test_homectl(Fixture *fixture, gconstpointer data)
{
    const gchar *program = data;
    g_autoptr(GSubprocessLauncher) launcher = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    g_subprocess_launcher_setenv(launcher, "DBUS_SYSTEM_BUS_ADDRESS",
                                 g_test_dbus_get_bus_address(fixture->bus), TRUE);
    const gchar *const commands[][5] = {
        { program, "--json", "list", NULL },
        { program, "--json", "inspect", "root", NULL },
        { program, "--json", "inspect", "0", NULL },
    };
    for (guint i = 0; i < G_N_ELEMENTS(commands); i++) {
        g_autoptr(GError) error = NULL;
        g_autoptr(GSubprocess) process = g_subprocess_launcher_spawnv(launcher, commands[i], &error);
        g_assert_no_error(error);
        g_autofree gchar *output = NULL;
        g_autofree gchar *diagnostic = NULL;
        g_assert_true(g_subprocess_communicate_utf8(process, NULL, NULL, &output, &diagnostic, &error));
        g_assert_no_error(error);
        g_assert_cmpstr(diagnostic, ==, "");
        g_assert_true(g_subprocess_get_successful(process));
        if (i != 0)
            assert_user_record_json(output);
        else {
            g_autoptr(JsonNode) node = json_from_string(output, &error);
            g_assert_no_error(error);
            g_assert_true(JSON_NODE_HOLDS_ARRAY(node));
            g_assert_cmpuint(json_array_get_length(json_node_get_array(node)), >, 0);
        }
    }
}

static void
test_mixed_nss_concurrency(Fixture *fixture, gconstpointer data)
{
    enum { THREADS = 8 };
    GThread *threads[THREADS];
    (void) data;

    for (guint i = 0; i < THREADS; i++)
        threads[i] = g_thread_new("mixed-nss-test", mixed_nss_worker, fixture);
    for (guint i = 0; i < THREADS; i++)
        g_assert_null(g_thread_join(threads[i]));
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/dbus/record-by-name", Fixture, NULL, setup,
               test_record_by_name, teardown);
    g_test_add("/dbus/nss-lock-contention", Fixture, NULL, setup,
               test_nss_lock_contention, teardown);
    g_test_add("/dbus/record-by-uid", Fixture, NULL, setup,
               test_record_by_uid, teardown);
    g_test_add("/dbus/record-by-caller", Fixture, NULL, setup,
               test_record_by_caller, teardown);
    g_test_add("/dbus/record-by-pid", Fixture, NULL, setup,
               test_record_by_pid, teardown);
    g_test_add("/dbus/invalid-pids", Fixture, NULL, setup,
               test_invalid_pids, teardown);
    g_test_add("/dbus/missing-user", Fixture, NULL, setup,
               test_missing_user, teardown);
    g_test_add("/dbus/list-homes", Fixture, NULL, setup,
               test_list_homes, teardown);
    g_test_add("/dbus/home-by-name", Fixture, NULL, setup,
               test_home_by_name, teardown);
    g_test_add("/dbus/home-by-uid", Fixture, NULL, setup,
               test_home_by_uid, teardown);
    g_test_add("/dbus/home-property", Fixture, NULL, setup,
               test_home_property, teardown);
    g_test_add("/dbus/unsupported-home-operation", Fixture, NULL, setup,
               test_unsupported_home_operation, teardown);
    g_test_add("/dbus/read-only-home-property", Fixture, NULL, setup,
               test_read_only_home_property, teardown);
    g_test_add("/dbus/unsupported-home-operations", Fixture, NULL, setup,
               test_unsupported_home_operations, teardown);
    g_test_add("/dbus/mixed-nss-concurrency", Fixture, NULL, setup,
               test_mixed_nss_concurrency, teardown);
    const gchar *homectl = g_getenv("HOMED_TEST_HOMECTL");
    if (homectl != NULL)
        g_test_add("/dbus/homectl", Fixture, homectl, setup, test_homectl, teardown);
    return g_test_run();
}
