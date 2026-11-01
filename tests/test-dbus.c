#include "rcl-home.h"

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
    if (home != NULL && reply != 0)
        rcl_home_register(home, connection, &error);

    g_mutex_lock(&fixture->mutex);
    fixture->startup_error = error;
    fixture->ready = TRUE;
    g_cond_signal(&fixture->cond);
    g_mutex_unlock(&fixture->mutex);

    if (error == NULL) {
        fixture->loop = g_main_loop_new(fixture->context, FALSE);
        g_main_loop_run(fixture->loop);
        g_main_loop_unref(fixture->loop);
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

static void
setup(Fixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    (void) data;

    g_mutex_init(&fixture->mutex);
    g_cond_init(&fixture->cond);
    fixture->bus = g_test_dbus_new(G_TEST_DBUS_NONE);
    g_test_dbus_up(fixture->bus);
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
    g_test_dbus_down(fixture->bus);
    g_object_unref(fixture->bus);
    g_clear_error(&fixture->startup_error);
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
assert_user_record(GVariant *reply)
{
    GVariant *record = NULL;
    gboolean incomplete = FALSE;
    guint32 uid = G_MAXUINT32;
    const gchar *name = NULL;

    g_variant_get(reply, "(@a{sv}b)", &record, &incomplete);
    g_assert_true(incomplete);
    g_assert_true(g_variant_lookup(record, "userName", "&s", &name));
    g_assert_cmpstr(name, ==, "root");
    g_assert_true(g_variant_lookup(record, "uid", "u", &uid));
    g_assert_cmpuint(uid, ==, 0);
    g_variant_unref(record);
}

static void
test_record_by_name(Fixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    GVariant *reply = call_manager(fixture, "GetUserRecordByName",
        g_variant_new("(s)", "root"), G_VARIANT_TYPE("(a{sv}b)"), &error);
    (void) data;
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    assert_user_record(reply);
    g_variant_unref(reply);
}

static void
test_record_by_uid(Fixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    GVariant *reply = call_manager(fixture, "GetUserRecordByUID",
        g_variant_new("(u)", 0u), G_VARIANT_TYPE("(a{sv}b)"), &error);
    (void) data;
    g_assert_no_error(error);
    g_assert_nonnull(reply);
    assert_user_record(reply);
    g_variant_unref(reply);
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

static void
test_missing_user(Fixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    GVariant *reply = call_manager(fixture, "GetUserRecordByName",
        g_variant_new("(s)", "homed-test-user-that-does-not-exist"),
        G_VARIANT_TYPE("(a{sv}b)"), &error);
    (void) data;
    g_assert_null(reply);
    g_assert_nonnull(error);
    g_assert_true(g_dbus_error_is_remote_error(error));
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
test_home_property(Fixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    GVariant *home = call_manager(fixture, "GetHomeByName",
        g_variant_new("(s)", "root"), G_VARIANT_TYPE("(usussso)"), &error);
    GVariant *path_value;
    const gchar *path;
    GVariant *reply, *boxed, *value;
    (void) data;
    g_assert_no_error(error);
    g_assert_nonnull(home);
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
    g_variant_unref(path_value);
    g_variant_unref(home);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/dbus/record-by-name", Fixture, NULL, setup,
               test_record_by_name, teardown);
    g_test_add("/dbus/record-by-uid", Fixture, NULL, setup,
               test_record_by_uid, teardown);
    g_test_add("/dbus/record-by-pid", Fixture, NULL, setup,
               test_record_by_pid, teardown);
    g_test_add("/dbus/missing-user", Fixture, NULL, setup,
               test_missing_user, teardown);
    g_test_add("/dbus/list-homes", Fixture, NULL, setup,
               test_list_homes, teardown);
    g_test_add("/dbus/home-by-name", Fixture, NULL, setup,
               test_home_by_name, teardown);
    g_test_add("/dbus/home-property", Fixture, NULL, setup,
               test_home_property, teardown);
    return g_test_run();
}
