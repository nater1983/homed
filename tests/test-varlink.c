#include "rcl-nss.h"
#include "rcl-varlink.h"
#include "config.h"

#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct {
    gchar *directory;
    gchar *socket_path;
    GSocketService *service;
    GMainLoop *loop;
    GThread *loop_thread;
} Fixture;

static Fixture fixture;

static gpointer
run_loop(gpointer data)
{
    g_main_loop_run(data);
    return NULL;
}

static void
setup(Fixture *unused, gconstpointer data)
{
    GError *error = NULL;
    (void) unused; (void) data;

    fixture.directory = g_dir_make_tmp("homed-varlink-test-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(fixture.directory);

    fixture.socket_path = g_build_filename(fixture.directory,
                                             "io.systemd.Home", NULL);
    fixture.service = rcl_varlink_start_at(fixture.directory,
                                            fixture.socket_path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(fixture.service);
    fixture.loop = g_main_loop_new(NULL, FALSE);
    fixture.loop_thread = g_thread_new("varlink-test-loop", run_loop,
                                       fixture.loop);
}

static void
teardown(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    rcl_varlink_stop(fixture.service);
    fixture.service = NULL;
    g_main_loop_quit(fixture.loop);
    g_thread_join(fixture.loop_thread);
    g_main_loop_unref(fixture.loop);
    g_rmdir(fixture.directory);
    g_clear_pointer(&fixture.socket_path, g_free);
    g_clear_pointer(&fixture.directory, g_free);
}

static gchar *
exchange(GSocketConnection *connection, const gchar *request)
{
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GByteArray *reply = g_byte_array_new();
    gchar *framed = g_strconcat(request, "\0", NULL);
    guint8 byte;
    GError *error = NULL;

    g_assert_true(g_output_stream_write_all(output, framed, strlen(framed) + 1,
                                            NULL, NULL, &error));
    g_assert_no_error(error);
    g_free(framed);

    do {
        gssize count = g_input_stream_read(input, &byte, 1, NULL, &error);
        g_assert_no_error(error);
        g_assert_cmpint(count, ==, 1);
        if (byte != 0)
            g_byte_array_append(reply, &byte, 1);
    } while (byte != 0);

    g_byte_array_append(reply, (const guint8 *)"", 1);
    return (gchar *)g_byte_array_free(reply, FALSE);
}

static GSocketConnection *
connect_socket(void)
{
    GSocketClient *client = g_socket_client_new();
    GSocketAddress *address = g_unix_socket_address_new(fixture.socket_path);
    GError *error = NULL;
    GSocketConnection *connection =
        g_socket_client_connect(client, G_SOCKET_CONNECTABLE(address), NULL, &error);

    g_assert_no_error(error);
    g_assert_nonnull(connection);
    g_object_unref(address);
    g_object_unref(client);
    return connection;
}

static void
assert_reply_contains(const gchar *reply, const gchar *needle)
{
    g_assert_nonnull(strstr(reply, needle));
}

static void
test_get_info(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();
    gchar *reply = exchange(connection,
        "{\"method\":\"org.varlink.service.GetInfo\",\"parameters\":{}}");
    gchar *vendor = g_strdup_printf("\"vendor\":\"%s\"", VARLINK_VENDOR);
    gchar *version = g_strdup_printf("\"version\":\"%s\"", PACKAGE_VERSION);

    assert_reply_contains(reply, vendor);
    assert_reply_contains(reply, "\"product\":\"homed\"");
    assert_reply_contains(reply, version);
    assert_reply_contains(reply, "\"io.systemd.UserDatabase\"");
    assert_reply_contains(reply, "\"continues\":false");
    g_free(vendor);
    g_free(version);
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}
static void
test_user_record(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();
    gchar *reply = exchange(connection,
        "{\"method\":\"io.systemd.UserDatabase.GetUserRecord\","
        "\"parameters\":{\"service\":\"io.systemd.Home\",\"userName\":\"root\"}}");

    assert_reply_contains(reply, "\"userName\":\"root\"");
    assert_reply_contains(reply, "\"uid\":0");
    assert_reply_contains(reply, "\"homeDirectory\":\"/root\"");
    assert_reply_contains(reply, "\"incomplete\":true");
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

static void
test_group_record(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();
    gchar *reply = exchange(connection,
        "{\"method\":\"io.systemd.UserDatabase.GetGroupRecord\","
        "\"parameters\":{\"service\":\"io.systemd.Home\",\"groupName\":\"root\"}}");

    assert_reply_contains(reply, "\"groupName\":\"root\"");
    assert_reply_contains(reply, "\"gid\":0");
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

static void
test_memberships(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();
    gchar *reply = exchange(connection,
        "{\"method\":\"io.systemd.UserDatabase.GetMemberships\","
        "\"parameters\":{\"service\":\"io.systemd.Home\","
        "\"userName\":\"root\",\"groupName\":\"root\"}}");

    assert_reply_contains(reply, "\"userName\":\"root\"");
    assert_reply_contains(reply, "\"groupName\":\"root\"");
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

static void
test_unknown_method(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();
    gchar *reply = exchange(connection,
        "{\"method\":\"io.systemd.UserDatabase.NoSuchMethod\","
        "\"parameters\":{}}");

    assert_reply_contains(reply, "org.varlink.service.MethodNotFound");
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

static void
test_bad_service(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();
    gchar *reply = exchange(connection,
        "{\"method\":\"io.systemd.UserDatabase.GetUserRecord\","
        "\"parameters\":{\"service\":\"wrong\",\"userName\":\"root\"}}");

    assert_reply_contains(reply, "io.systemd.UserDatabase.BadService");
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

static void
test_multiple_requests(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();
    gchar *user = exchange(connection,
        "{\"method\":\"io.systemd.UserDatabase.GetUserRecord\","
        "\"parameters\":{\"service\":\"io.systemd.Home\",\"userName\":\"root\"}}");
    gchar *group = exchange(connection,
        "{\"method\":\"io.systemd.UserDatabase.GetGroupRecord\","
        "\"parameters\":{\"service\":\"io.systemd.Home\",\"groupName\":\"root\"}}");

    assert_reply_contains(user, "\"userName\":\"root\"");
    assert_reply_contains(group, "\"groupName\":\"root\"");
    g_free(user);
    g_free(group);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}


static void
test_user_record_by_uid(Fixture *unused, gconstpointer data)
{
    GSocketConnection *connection = connect_socket();
    gchar *reply = exchange(connection,
        "{\"method\":\"io.systemd.UserDatabase.GetUserRecord\","
        "\"parameters\":{\"service\":\"io.systemd.Home\",\"uid\":0}}");
    (void) unused; (void) data;
    assert_reply_contains(reply, "\"userName\":\"root\"");
    assert_reply_contains(reply, "\"uid\":0");
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

static void
test_missing_user(Fixture *unused, gconstpointer data)
{
    GSocketConnection *connection = connect_socket();
    gchar *reply = exchange(connection,
        "{\"method\":\"io.systemd.UserDatabase.GetUserRecord\","
        "\"parameters\":{\"service\":\"io.systemd.Home\","
        "\"userName\":\"homed-test-user-that-does-not-exist\"}}");
    (void) unused; (void) data;
    assert_reply_contains(reply, "io.systemd.UserDatabase.NoRecordFound");
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

static void
test_conflicting_user_identity(Fixture *unused, gconstpointer data)
{
    GSocketConnection *connection = connect_socket();
    gchar *reply = exchange(connection,
        "{\"method\":\"io.systemd.UserDatabase.GetUserRecord\","
        "\"parameters\":{\"service\":\"io.systemd.Home\","
        "\"userName\":\"root\",\"uid\":1}}");
    (void) unused; (void) data;
    assert_reply_contains(reply, "io.systemd.UserDatabase.ConflictingRecordFound");
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

static void
test_invalid_json(Fixture *unused, gconstpointer data)
{
    GSocketConnection *connection = connect_socket();
    gchar *reply = exchange(connection, "{");

    (void) unused; (void) data;
    assert_reply_contains(reply, "org.varlink.service.InvalidParameter");
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

typedef struct {
    guint iterations;
    gint failures;
} Worker;

static gpointer
concurrent_worker(gpointer data)
{
    Worker *worker = data;

    for (guint i = 0; i < worker->iterations; i++) {
        GSocketConnection *connection = connect_socket();
        gchar *reply = exchange(connection,
            "{\"method\":\"io.systemd.UserDatabase.GetUserRecord\","
            "\"parameters\":{\"service\":\"io.systemd.Home\",\"userName\":\"root\"}}");

        if (strstr(reply, "\"userName\":\"root\"") == NULL)
            g_atomic_int_inc(&worker->failures);
        g_free(reply);
        g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
        g_object_unref(connection);
    }

    return NULL;
}

static void
test_concurrent_requests(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    enum { THREADS = 8, ITERATIONS = 10 };
    Worker worker = { .iterations = ITERATIONS, .failures = 0 };
    GThread *threads[THREADS];

    for (guint i = 0; i < THREADS; i++)
        threads[i] = g_thread_new("varlink-test", concurrent_worker, &worker);
    for (guint i = 0; i < THREADS; i++)
        g_thread_join(threads[i]);

    g_assert_cmpint(g_atomic_int_get(&worker.failures), ==, 0);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/varlink/get-info", Fixture, NULL, setup,
               test_get_info, teardown);
    g_test_add("/varlink/user-record", Fixture, NULL, setup,
               test_user_record, teardown);
    g_test_add("/varlink/group-record", Fixture, NULL, setup,
               test_group_record, teardown);
    g_test_add("/varlink/user-record-by-uid", Fixture, NULL, setup,
               test_user_record_by_uid, teardown);
    g_test_add("/varlink/missing-user", Fixture, NULL, setup,
               test_missing_user, teardown);
    g_test_add("/varlink/conflicting-user-identity", Fixture, NULL, setup,
               test_conflicting_user_identity, teardown);
    g_test_add("/varlink/memberships", Fixture, NULL, setup,
               test_memberships, teardown);
    g_test_add("/varlink/unknown-method", Fixture, NULL, setup,
               test_unknown_method, teardown);
    g_test_add("/varlink/bad-service", Fixture, NULL, setup,
               test_bad_service, teardown);
    g_test_add("/varlink/invalid-json", Fixture, NULL, setup,
               test_invalid_json, teardown);
    g_test_add("/varlink/multiple-requests", Fixture, NULL, setup,
               test_multiple_requests, teardown);
    g_test_add("/varlink/concurrent-requests", Fixture, NULL, setup,
               test_concurrent_requests, teardown);
    return g_test_run();
}
