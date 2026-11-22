#include "rcl-nss.h"
#include "rcl-varlink.h"
#include "rcl-varlink-server.h"
#include "config.h"

#include <gio/gunixsocketaddress.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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
    /* The listener's cancelled async accept must be dispatched on the
     * default context before the final references are released. */
    while (g_main_context_iteration(NULL, FALSE))
        ;
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
test_invalid_more_type(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();
    const gchar *requests[] = {
        "{\"method\":\"org.varlink.service.GetInfo\",\"parameters\":{},\"more\":\"true\"}",
        "{\"method\":\"org.varlink.service.GetInfo\",\"parameters\":{},\"more\":1}",
        "{\"method\":\"org.varlink.service.GetInfo\",\"parameters\":{},\"more\":null}"
    };
    for (guint i = 0; i < G_N_ELEMENTS(requests); i++) {
        gchar *reply = exchange(connection, requests[i]);
        assert_reply_contains(reply, "org.varlink.service.InvalidParameter");
        g_free(reply);
    }
    gchar *reply = exchange(connection,
        "{\"method\":\"org.varlink.service.GetInfo\",\"parameters\":{},\"more\":false}");
    assert_reply_contains(reply, "\"product\":\"homed\"");
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

static void
test_get_info(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();
    gchar *reply = exchange(connection,
        "{\"method\":\"org.varlink.service.GetInfo\"}");
    gchar *vendor = g_strdup_printf("\"vendor\":\"%s\"", VARLINK_VENDOR);
    gchar *version = g_strdup_printf("\"version\":\"%s\"", PACKAGE_VERSION);

    assert_reply_contains(reply, vendor);
    assert_reply_contains(reply, "\"product\":\"homed\"");
    assert_reply_contains(reply, version);
    assert_reply_contains(reply, "\"io.systemd.UserDatabase\"");
    assert_reply_contains(reply, "\"org.varlink.service\"");
    assert_reply_contains(reply, "\"continues\":false");
    g_free(vendor);
    g_free(version);
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

static void
test_interface_description(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    g_autoptr(GSocketConnection) connection = connect_socket();
    const gchar *interfaces[] = { "io.systemd.UserDatabase", "org.varlink.service", "io.example.Missing" };
    const gchar *expected[] = { "method GetMemberships(", "method GetInterfaceDescription(",
                                "org.varlink.service.InterfaceNotFound" };

    for (guint i = 0; i < G_N_ELEMENTS(interfaces); i++) {
        g_autofree gchar *request = g_strdup_printf(
            "{\"method\":\"org.varlink.service.GetInterfaceDescription\","
            "\"parameters\":{\"interface\":\"%s\"}}", interfaces[i]);
        g_autofree gchar *reply = exchange(connection, request);
        assert_reply_contains(reply, expected[i]);
        assert_reply_contains(reply, interfaces[i]);
    }
}

static void
test_lookup_parameter_validation(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    g_autoptr(GSocketConnection) connection = connect_socket();
    const struct {
        const gchar *method, *parameters, *invalid;
    } cases[] = {
        { "GetUserRecord", "\"userName\":\"root\",\"uidMin\":1000", "uidMin" },
        { "GetUserRecord", "\"uid\":-1", "uid" },
        { "GetUserRecord", "\"uid\":4294967296", "uid" },
        { "GetGroupRecord", "\"groupName\":\"root\",\"fuzzyNames\":[]", "fuzzyNames" },
        { "GetMemberships", "\"userName\":true,\"groupName\":\"root\"", "userName" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        g_autofree gchar *request = g_strdup_printf(
            "{\"method\":\"io.systemd.UserDatabase.%s\","
            "\"parameters\":{\"service\":\"io.systemd.Home\",%s}}",
            cases[i].method, cases[i].parameters);
        g_autofree gchar *reply = exchange(connection, request);
        g_autofree gchar *detail = g_strdup_printf("\"parameter\":\"%s\"", cases[i].invalid);
        assert_reply_contains(reply, "org.varlink.service.InvalidParameter");
        assert_reply_contains(reply, detail);
    }

    g_autofree gchar *reply = exchange(connection,
        "{\"method\":\"io.systemd.UserDatabase.GetUserRecord\","
        "\"parameters\":{\"service\":\"io.systemd.Home\",\"uid\":0,\"userName\":null}}");
    assert_reply_contains(reply, "\"userName\":\"root\"");
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
    assert_reply_contains(reply, "\"incomplete\":false");
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
test_group_record_by_gid(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();
    gchar *reply = exchange(connection,
        "{\"method\":\"io.systemd.UserDatabase.GetGroupRecord\","
        "\"parameters\":{\"service\":\"io.systemd.Home\",\"gid\":0}}");

    assert_reply_contains(reply, "\"groupName\":\"root\"");
    assert_reply_contains(reply, "\"gid\":0");
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

static void
test_conflicting_group_identity(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();
    gchar *reply = exchange(connection,
        "{\"method\":\"io.systemd.UserDatabase.GetGroupRecord\","
        "\"parameters\":{\"service\":\"io.systemd.Home\","
        "\"groupName\":\"root\",\"gid\":1}}");

    assert_reply_contains(reply, "io.systemd.UserDatabase.ConflictingRecordFound");
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
    g_autoptr(GSocketConnection) connection = connect_socket();
    const gchar *requests[] = {
        "{\"method\":\"io.systemd.UserDatabase.GetUserRecord\","
        "\"parameters\":{\"service\":\"wrong\",\"userName\":\"root\"}}",
        "{\"method\":\"io.systemd.UserDatabase.GetGroupRecord\","
        "\"parameters\":{\"groupName\":\"root\"}}",
        "{\"method\":\"io.systemd.UserDatabase.GetMemberships\"}",
    };
    for (guint i = 0; i < G_N_ELEMENTS(requests); i++) {
        g_autofree gchar *reply = exchange(connection, requests[i]);
        assert_reply_contains(reply, "io.systemd.UserDatabase.BadService");
    }
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



static gchar *
read_reply(GInputStream *input)
{
    GByteArray *reply = g_byte_array_new();
    guint8 byte;
    GError *error = NULL;

    do {
        gssize count = g_input_stream_read(input, &byte, 1, NULL, &error);
        g_assert_no_error(error);
        g_assert_cmpint(count, ==, 1);
        if (byte != 0)
            g_byte_array_append(reply, &byte, 1);
    } while (byte != 0);

    g_byte_array_append(reply, (const guint8 *)"", 1);
    return (gchar *) g_byte_array_free(reply, FALSE);
}

static GPtrArray *
exchange_memberships(GSocketConnection *connection, const gchar *parameters)
{
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    gchar *request = g_strdup_printf(
        "{\"method\":\"io.systemd.UserDatabase.GetMemberships\","
        "\"parameters\":{%s},\"more\":true}", parameters);
    gchar *framed = g_strconcat(request, "\0", NULL);
    GError *error = NULL;
    GPtrArray *replies = g_ptr_array_new_with_free_func(g_free);

    g_assert_true(g_output_stream_write_all(output, framed, strlen(framed) + 1,
                                            NULL, NULL, &error));
    g_assert_no_error(error);
    g_free(request);
    g_free(framed);

    for (;;) {
        gchar *reply = read_reply(input);
        JsonParser *parser = json_parser_new();
        JsonObject *object;
        gboolean continues;

        g_assert_true(json_parser_load_from_data(parser, reply, -1, &error));
        g_assert_no_error(error);
        object = json_node_get_object(json_parser_get_root(parser));
        continues = json_object_get_boolean_member(object, "continues");
        g_ptr_array_add(replies, reply);
        g_object_unref(parser);
        if (!continues)
            break;
    }
    return replies;
}

static void
test_membership_consistency(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();
    g_autofree gchar *reply = exchange(connection,
        "{\"method\":\"io.systemd.UserDatabase.GetMemberships\","
        "\"parameters\":{\"service\":\"io.systemd.Home\",\"userName\":\"root\"}}");
    assert_reply_contains(reply, "org.varlink.service.ExpectedMore");

    GPtrArray *user_replies = exchange_memberships(connection,
        "\"service\":\"io.systemd.Home\",\"userName\":\"root\"");
    gboolean found_root_group = FALSE;

    g_assert_cmpuint(user_replies->len, >, 0);
    for (guint i = 0; i < user_replies->len; i++) {
        JsonParser *parser = json_parser_new();
        JsonObject *response, *parameters;
        g_assert_true(json_parser_load_from_data(parser,
            g_ptr_array_index(user_replies, i), -1, NULL));
        response = json_node_get_object(json_parser_get_root(parser));
        parameters = json_object_get_object_member(response, "parameters");
        g_assert_cmpstr(json_object_get_string_member(parameters, "userName"),
                        ==, "root");
        if (g_str_equal(json_object_get_string_member(parameters, "groupName"),
                        "root"))
            found_root_group = TRUE;
        g_object_unref(parser);
    }
    g_assert_true(found_root_group);

    GPtrArray *group_replies = exchange_memberships(connection,
        "\"service\":\"io.systemd.Home\",\"groupName\":\"root\"");
    gboolean found_root_user = FALSE;

    g_assert_cmpuint(group_replies->len, >, 0);
    for (guint i = 0; i < group_replies->len; i++) {
        JsonParser *parser = json_parser_new();
        JsonObject *response, *parameters;
        g_assert_true(json_parser_load_from_data(parser,
            g_ptr_array_index(group_replies, i), -1, NULL));
        response = json_node_get_object(json_parser_get_root(parser));
        parameters = json_object_get_object_member(response, "parameters");
        g_assert_cmpstr(json_object_get_string_member(parameters, "groupName"),
                        ==, "root");
        if (g_str_equal(json_object_get_string_member(parameters, "userName"),
                        "root"))
            found_root_user = TRUE;
        g_object_unref(parser);
    }
    g_assert_true(found_root_user);
    g_ptr_array_unref(user_replies);
    g_ptr_array_unref(group_replies);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

static void
test_combined_membership_identity(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();
    GPtrArray *replies = exchange_memberships(connection,
        "\"service\":\"io.systemd.Home\",\"userName\":\"root\",\"groupName\":\"root\"");

    g_assert_cmpuint(replies->len, ==, 1);
    JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser,
        g_ptr_array_index(replies, 0), -1, NULL));
    JsonObject *response = json_node_get_object(json_parser_get_root(parser));
    JsonObject *parameters = json_object_get_object_member(response, "parameters");
    g_assert_cmpstr(json_object_get_string_member(parameters, "userName"), ==, "root");
    g_assert_cmpstr(json_object_get_string_member(parameters, "groupName"), ==, "root");

    g_object_unref(parser);
    g_ptr_array_unref(replies);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

static void
test_membership_nss_failures(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    static const gchar *filters[] = {
        "\"userName\":\"root\"",
        "\"userName\":\"root\",\"groupName\":\"root\"",
        "\"groupName\":\"root\""
    };

    rcl_varlink_test_fail_group_list(TRUE);
    for (guint i = 0; i < G_N_ELEMENTS(filters); i++) {
        GSocketConnection *connection = connect_socket();
        gchar *request = g_strdup_printf(
            "{\"method\":\"io.systemd.UserDatabase.GetMemberships\",\"more\":true,"
            "\"parameters\":{\"service\":\"io.systemd.Home\",%s}}", filters[i]);
        gchar *reply = exchange(connection, request);
        assert_reply_contains(reply, "io.systemd.UserDatabase.ServiceNotAvailable");
        g_free(reply);
        g_free(request);
        g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
        g_object_unref(connection);
    }
    rcl_varlink_test_fail_group_list(FALSE);

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

/* The first response is held after NSS unlock but before socket I/O.
 * A second request must complete while that response remains paused. */
extern void rcl_varlink_test_set_before_nss_reply_write(void (*callback)(void));

static GMutex slow_reply_mutex;
static GCond slow_reply_cond;
static gboolean slow_reply_paused;
static gboolean slow_reply_release;
static gint slow_reply_callbacks;

static void
pause_first_nss_reply(void)
{
    if (!g_atomic_int_compare_and_exchange(&slow_reply_callbacks, 0, 1))
        return;

    g_mutex_lock(&slow_reply_mutex);
    slow_reply_paused = TRUE;
    g_cond_broadcast(&slow_reply_cond);
    while (!slow_reply_release)
        g_cond_wait(&slow_reply_cond, &slow_reply_mutex);
    g_mutex_unlock(&slow_reply_mutex);
}

static gpointer
slow_reply_worker(gpointer unused)
{
    (void) unused;
    g_autoptr(GSocketConnection) connection = connect_socket();
    g_autofree gchar *reply = exchange(connection,
        "{\\"method\\":\\"io.systemd.UserDatabase.GetUserRecord\\","
        "\\"parameters\\":{\\"service\\":\\"io.systemd.Home\\","
        "\\"userName\\":\\"root\\"}}");
    assert_reply_contains(reply, "\\"userName\\":\\"root\\"");
    return NULL;
}

static void
test_slow_reply_does_not_hold_nss_lock(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    g_mutex_init(&slow_reply_mutex);
    g_cond_init(&slow_reply_cond);
    slow_reply_paused = FALSE;
    slow_reply_release = FALSE;
    g_atomic_int_set(&slow_reply_callbacks, 0);
    rcl_varlink_test_set_before_nss_reply_write(pause_first_nss_reply);

    GThread *first = g_thread_new("paused-varlink-reply",
                                  slow_reply_worker, NULL);

    g_mutex_lock(&slow_reply_mutex);
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!slow_reply_paused && g_get_monotonic_time() < deadline)
        g_cond_wait_until(&slow_reply_cond, &slow_reply_mutex, deadline);
    gboolean paused = slow_reply_paused;
    g_mutex_unlock(&slow_reply_mutex);

    if (paused) {
        g_autoptr(GSocketConnection) second = connect_socket();
        g_autofree gchar *reply = exchange(second,
            "{\\"method\\":\\"io.systemd.UserDatabase.GetUserRecord\\","
            "\\"parameters\\":{\\"service\\":\\"io.systemd.Home\\","
            "\\"userName\\":\\"root\\"}}");
        assert_reply_contains(reply, "\\"userName\\":\\"root\\"");
    }

    g_mutex_lock(&slow_reply_mutex);
    slow_reply_release = TRUE;
    g_cond_broadcast(&slow_reply_cond);
    g_mutex_unlock(&slow_reply_mutex);
    g_thread_join(first);
    rcl_varlink_test_set_before_nss_reply_write(NULL);
    g_cond_clear(&slow_reply_cond);
    g_mutex_clear(&slow_reply_mutex);
    g_assert_true(paused);
}

static void
test_nss_lock_contention(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();

    rcl_nss_lock();
    gchar *reply = exchange(connection,
        "{\"method\":\"io.systemd.UserDatabase.GetUserRecord\","
        "\"parameters\":{\"service\":\"io.systemd.Home\","
        "\"userName\":\"root\"}}");
    rcl_nss_unlock();

    assert_reply_contains(reply,
        "io.systemd.UserDatabase.ServiceNotAvailable");
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

static void
simulate_nss_provider_callback(void)
{
    GSocketConnection *connection = connect_socket();
    gchar *reply = exchange(connection,
        "{\"method\":\"io.systemd.UserDatabase.GetUserRecord\","
        "\"parameters\":{\"service\":\"io.systemd.Home\","
        "\"userName\":\"root\"}}");

    assert_reply_contains(reply,
        "io.systemd.UserDatabase.ServiceNotAvailable");
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

static void
test_nss_provider_recursion(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();
    gchar *reply;

    rcl_nss_test_set_callback(simulate_nss_provider_callback);
    reply = exchange(connection,
        "{\"method\":\"io.systemd.UserDatabase.GetUserRecord\","
        "\"parameters\":{\"service\":\"io.systemd.Home\","
        "\"userName\":\"root\"}}");

    assert_reply_contains(reply, "\"userName\":\"root\"");
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

static void
test_listener_path_safety(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *base = g_dir_make_tmp("homed-varlink-path-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(base);

    g_autofree gchar *directory = g_build_filename(base, "socket-dir", NULL);
    g_autofree gchar *socket_path = g_build_filename(directory, "io.systemd.Home", NULL);
    g_autofree gchar *alias = g_build_filename(base, "alias", NULL);
    g_autofree gchar *alias_socket = g_build_filename(alias, "io.systemd.Home", NULL);
    g_assert_cmpint(g_mkdir(directory, 0700), ==, 0);
    g_assert_cmpint(symlink(directory, alias), ==, 0);

    g_assert_null(rcl_varlink_start_at(alias, alias_socket, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    g_assert_cmpint(g_chmod(base, 0777), ==, 0);
    g_assert_null(rcl_varlink_start_at(directory, socket_path, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    g_assert_cmpint(g_chmod(base, 0700), ==, 0);

    g_assert_cmpint(g_chmod(directory, 01777), ==, 0);
    g_assert_null(rcl_varlink_start_at(directory, socket_path, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    g_assert_cmpint(g_chmod(directory, 0700), ==, 0);

    g_assert_true(g_file_set_contents(socket_path, "keep", -1, &error));
    g_assert_no_error(error);
    g_assert_null(rcl_varlink_start_at(directory, socket_path, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);
    g_autofree gchar *contents = NULL;
    g_assert_true(g_file_get_contents(socket_path, &contents, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(contents, ==, "keep");
    g_assert_cmpint(g_unlink(socket_path), ==, 0);
    g_assert_cmpint(g_unlink(alias), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    g_assert_cmpint(g_rmdir(base), ==, 0);
}

static void
test_listener_elogind_alias(void)
{
    g_autoptr(GError) error = NULL;
    g_autofree gchar *runtime = g_dir_make_tmp("homed-varlink-elogind-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(runtime);

    g_autofree gchar *systemd = g_build_filename(runtime, "systemd", NULL);
    g_autofree gchar *elogind = g_build_filename(runtime, "elogind", NULL);
    g_autofree gchar *directory = g_build_filename(elogind, "userdb", NULL);
    g_autofree gchar *socket_path = g_build_filename(directory, "io.systemd.Home", NULL);
    g_assert_cmpint(g_mkdir(elogind, 0755), ==, 0);
    g_assert_cmpint(g_mkdir(directory, 0755), ==, 0);
    g_assert_cmpint(symlink("elogind", systemd), ==, 0);

    /* The alias must reach the socket-path check in the physical directory. */
    g_assert_true(g_file_set_contents(socket_path, "keep", -1, &error));
    g_assert_no_error(error);
    g_assert_null(rcl_varlink_server_start(runtime, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);

    g_assert_cmpint(g_chmod(elogind, 0777), ==, 0);
    g_assert_null(rcl_varlink_server_start(runtime, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    g_assert_cmpint(g_chmod(elogind, 0755), ==, 0);

    g_assert_cmpint(g_unlink(systemd), ==, 0);
    g_assert_cmpint(symlink(elogind, systemd), ==, 0);
    g_assert_null(rcl_varlink_server_start(runtime, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);

    g_assert_cmpint(g_unlink(systemd), ==, 0);
    g_assert_cmpint(symlink("elogind/userdb", systemd), ==, 0);
    g_assert_null(rcl_varlink_server_start(runtime, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    g_assert_cmpint(g_unlink(socket_path), ==, 0);
    g_assert_cmpint(g_unlink(systemd), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    g_assert_cmpint(g_rmdir(elogind), ==, 0);
    g_assert_cmpint(g_rmdir(runtime), ==, 0);
}

static void
test_listener_active_socket(Fixture *unused, gconstpointer data)
{
    struct stat status;
    g_autoptr(GError) error = NULL;
    (void) unused; (void) data;

    g_assert_cmpint(lstat(fixture.socket_path, &status), ==, 0);
    g_assert_true(S_ISSOCK(status.st_mode));
    g_assert_cmpuint(status.st_mode & 0777, ==, 0666);

    g_assert_null(rcl_varlink_start_at(fixture.directory,
                                      fixture.socket_path, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_ADDRESS_IN_USE);

    g_autoptr(GSocketConnection) connection = connect_socket();
    g_autofree gchar *reply = exchange(connection,
        "{\"method\":\"org.varlink.service.GetInfo\",\"parameters\":{}}");
    assert_reply_contains(reply, "\"product\":\"homed\"");
}

static void
test_listener_stale_socket(Fixture *unused, gconstpointer data)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GSocket) socket = NULL;
    g_autoptr(GSocketAddress) address = NULL;
    (void) unused; (void) data;

    rcl_varlink_stop(fixture.service);
    fixture.service = NULL;
    socket = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                          G_SOCKET_PROTOCOL_DEFAULT, &error);
    g_assert_no_error(error);
    address = g_unix_socket_address_new(fixture.socket_path);
    g_assert_true(g_socket_bind(socket, address, FALSE, &error));
    g_assert_no_error(error);
    g_assert_true(g_socket_close(socket, &error));
    g_assert_no_error(error);

    fixture.service = rcl_varlink_start_at(fixture.directory,
                                          fixture.socket_path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(fixture.service);

    g_autoptr(GSocketConnection) connection = connect_socket();
    g_autofree gchar *reply = exchange(connection,
        "{\"method\":\"org.varlink.service.GetInfo\",\"parameters\":{}}");
    assert_reply_contains(reply, "\"product\":\"homed\"");
}

static void
test_listener_restart(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GError *error = NULL;
    GSocketConnection *connection = connect_socket();
    gchar *reply = exchange(connection,
        "{\"method\":\"org.varlink.service.GetInfo\",\"parameters\":{}}");
    assert_reply_contains(reply, "\"product\":\"homed\"");
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);

    rcl_varlink_stop(fixture.service);
    fixture.service = NULL;
    g_assert_false(g_file_test(fixture.socket_path, G_FILE_TEST_EXISTS));
    fixture.service = rcl_varlink_start_at(fixture.directory,
                                            fixture.socket_path, &error);
    g_assert_no_error(error);
    g_assert_nonnull(fixture.service);
    g_assert_true(g_file_test(fixture.socket_path, G_FILE_TEST_EXISTS));

    connection = connect_socket();
    reply = exchange(connection,
        "{\"method\":\"io.systemd.UserDatabase.GetUserRecord\","
        "\"parameters\":{\"service\":\"io.systemd.Home\",\"userName\":\"root\"}}");
    assert_reply_contains(reply, "\"userName\":\"root\"");
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

static void
test_pipelined_requests(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    const gchar requests[] =
        "{\"method\":\"io.systemd.UserDatabase.GetUserRecord\","
        "\"parameters\":{\"service\":\"io.systemd.Home\","
        "\"userName\":\"root\"}}"
        "\0"
        "{\"method\":\"io.systemd.UserDatabase.GetGroupRecord\","
        "\"parameters\":{\"service\":\"io.systemd.Home\","
        "\"groupName\":\"root\"}}"
        "\0";
    GError *error = NULL;

    g_assert_true(g_output_stream_write_all(output, requests,
        sizeof(requests) - 1, NULL, NULL, &error));
    g_assert_no_error(error);

    gchar *user = read_reply(input);
    gchar *group = read_reply(input);
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

static void
test_empty_frame(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    const guint8 empty = 0;
    GError *error = NULL;
    g_assert_true(g_output_stream_write_all(output, &empty, 1, NULL, NULL, &error));
    g_assert_no_error(error);
    gchar *reply = read_reply(input);
    g_assert_nonnull(strstr(reply, "org.varlink.service.InvalidParameter"));
    g_free(reply);
    reply = exchange(connection, "{\"method\":\"org.varlink.service.GetInfo\",\"parameters\":{}}");
    g_assert_nonnull(strstr(reply, "\"product\":\"homed\""));
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}



static void
test_oversized_frame(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GError *error = NULL;
    /* One byte beyond the maximum accepted 1 MiB payload. */
    gchar *payload = g_malloc0(1024U * 1024U + 2);
    memset(payload, 'x', 1024U * 1024U + 1);
    g_assert_true(g_output_stream_write_all(output, payload,
                                            1024U * 1024U + 1,
                                            NULL, NULL, &error));
    g_assert_no_error(error);
    gchar *reply = read_reply(input);
    assert_reply_contains(reply, "org.varlink.service.InvalidParameter");
    g_free(reply);
    guint8 byte;
    gssize count = g_input_stream_read(input, &byte, 1, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpint(count, ==, 0);
    g_free(payload);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

static void
test_incomplete_frame_disconnect(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    GError *error = NULL;
    const gchar *partial = "{\"method\":\"org.varlink.service.GetInfo\"";
    g_assert_true(g_output_stream_write_all(output, partial, strlen(partial),
                                            NULL, NULL, &error));
    g_assert_no_error(error);
    /* EOF before the NUL terminator must not dispatch the partial request. */
    g_assert_true(g_socket_shutdown(g_socket_connection_get_socket(connection),
                                    FALSE, TRUE, &error));
    g_assert_no_error(error);
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    guint8 byte;
    gssize count = g_input_stream_read(input, &byte, 1, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpint(count, ==, 0);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}


static void
test_disconnect_recovery(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    /* Disconnect repeatedly, including with an unfinished frame. */
    for (guint i = 0; i < 32; i++) {
        GSocketConnection *connection = connect_socket();
        GOutputStream *output =
            g_io_stream_get_output_stream(G_IO_STREAM(connection));
        const gchar *partial = "{";
        GError *error = NULL;
        g_assert_true(g_output_stream_write_all(output, partial,
                                                strlen(partial), NULL,
                                                NULL, &error));
        g_assert_no_error(error);
        g_assert_true(g_io_stream_close(G_IO_STREAM(connection), NULL, &error));
        g_assert_no_error(error);
        g_object_unref(connection);
        /* Allow the listener to accept and retire the closed connection.
         * A tight burst can exhaust the Unix socket's accept backlog,
         * which tests backlog capacity rather than disconnect recovery. */
        g_usleep(20000);
    }

    /* A subsequent connection must still serve a valid request. */
    GSocketConnection *connection = connect_socket();
    gchar *reply = exchange(connection,
        "{\"method\":\"org.varlink.service.GetInfo\",\"parameters\":{}}");
    assert_reply_contains(reply, "\"product\":\"homed\"");
    g_free(reply);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}


static void
test_shutdown_active_connection(Fixture *unused, gconstpointer data)
{
    (void) unused; (void) data;
    GSocketConnection *connection = connect_socket();
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    GError *error = NULL;
    /* Ensure the server has accepted this connection before shutdown. */
    gchar *reply = exchange(connection,
        "{\"method\":\"org.varlink.service.GetInfo\",\"parameters\":{}}");
    assert_reply_contains(reply, "\"product\":\"homed\"");
    g_free(reply);
    const gchar *partial = "{";
    g_assert_true(g_output_stream_write_all(output, partial, 1,
                                            NULL, NULL, &error));
    g_assert_no_error(error);

    /* The listener must close the idle worker and return without hanging. */
    rcl_varlink_stop(fixture.service);
    fixture.service = NULL;
    guint8 byte;
    gssize count = g_input_stream_read(input, &byte, 1, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpint(count, ==, 0);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

/* Keep all worker slots occupied by idle, accepted connections.  Each
 * GetInfo reply confirms that the listener dispatched that connection. */
static void
test_worker_limit_recovery(Fixture *unused, gconstpointer data)
{
    enum { LIMIT = 128 };
    GSocketConnection *held[LIMIT] = { NULL };
    GSocketConnection *overflow;
    GSocket *socket;
    GInputStream *input;
    guint8 byte;
    GError *error = NULL;
    gssize count;

    (void) unused; (void) data;

    for (guint i = 0; i < LIMIT; i++) {
        gchar *reply;
        held[i] = connect_socket();
        reply = exchange(held[i],
            "{\"method\":\"org.varlink.service.GetInfo\",\"parameters\":{}}");
        assert_reply_contains(reply, "\"product\":\"homed\"");
        g_free(reply);
    }

    /* Admission must close the 129th connection without spawning a worker.
     * Bound the read so a broken rejection path fails rather than hangs. */
    overflow = connect_socket();
    socket = g_socket_connection_get_socket(overflow);
    g_socket_set_timeout(socket, 5);
    input = g_io_stream_get_input_stream(G_IO_STREAM(overflow));
    count = g_input_stream_read(input, &byte, 1, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpint(count, ==, 0);
    g_io_stream_close(G_IO_STREAM(overflow), NULL, NULL);
    g_object_unref(overflow);

    /* Release one worker and verify that a fresh connection is admitted.
     * The worker exits asynchronously, so allow bounded time for cleanup. */
    g_io_stream_close(G_IO_STREAM(held[0]), NULL, NULL);
    g_clear_object(&held[0]);
    g_usleep(100000);

    {
        GSocketConnection *recovered = connect_socket();
        gchar *reply = exchange(recovered,
            "{\"method\":\"org.varlink.service.GetInfo\",\"parameters\":{}}");
        assert_reply_contains(reply, "\"product\":\"homed\"");
        g_free(reply);
        g_io_stream_close(G_IO_STREAM(recovered), NULL, NULL);
        g_object_unref(recovered);
    }

    for (guint i = 1; i < LIMIT; i++) {
        g_io_stream_close(G_IO_STREAM(held[i]), NULL, NULL);
        g_object_unref(held[i]);
    }
}

/* Verify shutdown wakes every idle worker without waiting for clients to
 * disconnect first.  The fixture teardown tolerates an already stopped
 * listener. */
static void
test_shutdown_saturated(Fixture *unused, gconstpointer data)
{
    enum { LIMIT = 128 };
    GSocketConnection *held[LIMIT] = { NULL };
    (void) unused; (void) data;

    for (guint i = 0; i < LIMIT; i++) {
        gchar *reply;
        held[i] = connect_socket();
        reply = exchange(held[i],
            "{\"method\":\"org.varlink.service.GetInfo\",\"parameters\":{}}");
        assert_reply_contains(reply, "\"product\":\"homed\"");
        g_free(reply);
    }
    rcl_varlink_stop(fixture.service);
    fixture.service = NULL;
    for (guint i = 0; i < LIMIT; i++) {
        guint8 byte;
        GError *error = NULL;
        GSocket *socket = g_socket_connection_get_socket(held[i]);
        g_socket_set_timeout(socket, 5);
        gssize count = g_input_stream_read(
            g_io_stream_get_input_stream(G_IO_STREAM(held[i])),
            &byte, 1, NULL, &error);
        g_assert_no_error(error);
        g_assert_cmpint(count, ==, 0);
        g_io_stream_close(G_IO_STREAM(held[i]), NULL, NULL);
        g_object_unref(held[i]);
    }
}

/* Repeat full admission and retirement to catch stale per-UID counters or
 * worker slots after a previous saturation cycle. */
static void
test_repeated_saturation(Fixture *unused, gconstpointer data)
{
    enum { LIMIT = 128, CYCLES = 3 };
    (void) unused; (void) data;

    for (guint cycle = 0; cycle < CYCLES; cycle++) {
        GSocketConnection *held[LIMIT] = { NULL };
        for (guint i = 0; i < LIMIT; i++) {
            gchar *reply;
            held[i] = connect_socket();
            reply = exchange(held[i],
                "{\"method\":\"org.varlink.service.GetInfo\",\"parameters\":{}}");
            assert_reply_contains(reply, "\"product\":\"homed\"");
            g_free(reply);
        }
        for (guint i = 0; i < LIMIT; i++) {
            g_io_stream_close(G_IO_STREAM(held[i]), NULL, NULL);
            g_object_unref(held[i]);
        }
        /* Worker cleanup runs asynchronously. */
        g_usleep(200000);
    }
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/varlink/shutdown-active-connection", Fixture, NULL, setup,
               test_shutdown_active_connection, teardown);
    g_test_add("/varlink/disconnect-recovery", Fixture, NULL, setup,
               test_disconnect_recovery, teardown);
    g_test_add("/varlink/oversized-frame", Fixture, NULL, setup,
               test_oversized_frame, teardown);
    g_test_add("/varlink/incomplete-frame-disconnect", Fixture, NULL, setup,
               test_incomplete_frame_disconnect, teardown);
    g_test_add("/varlink/empty-frame", Fixture, NULL, setup,
               test_empty_frame, teardown);
    g_test_add("/varlink/invalid-more-type", Fixture, NULL, setup,
               test_invalid_more_type, teardown);
    g_test_add("/varlink/get-info", Fixture, NULL, setup,
               test_get_info, teardown);
    g_test_add("/varlink/interface-description", Fixture, NULL, setup,
               test_interface_description, teardown);
    g_test_add("/varlink/lookup-parameter-validation", Fixture, NULL, setup,
               test_lookup_parameter_validation, teardown);
    g_test_add("/varlink/user-record", Fixture, NULL, setup,
               test_user_record, teardown);
    g_test_add("/varlink/group-record", Fixture, NULL, setup,
               test_group_record, teardown);
    g_test_add("/varlink/group-record-by-gid", Fixture, NULL, setup,
               test_group_record_by_gid, teardown);
    g_test_add("/varlink/conflicting-group-identity", Fixture, NULL, setup,
               test_conflicting_group_identity, teardown);
    g_test_add("/varlink/user-record-by-uid", Fixture, NULL, setup,
               test_user_record_by_uid, teardown);
    g_test_add("/varlink/missing-user", Fixture, NULL, setup,
               test_missing_user, teardown);
    g_test_add("/varlink/conflicting-user-identity", Fixture, NULL, setup,
               test_conflicting_user_identity, teardown);
    g_test_add("/varlink/membership-consistency", Fixture, NULL, setup,
               test_membership_consistency, teardown);
    g_test_add("/varlink/combined-membership-identity", Fixture, NULL, setup,
               test_combined_membership_identity, teardown);
    g_test_add("/varlink/listener-restart", Fixture, NULL, setup,
               test_listener_restart, teardown);
    g_test_add_func("/varlink/listener-path-safety", test_listener_path_safety);
    g_test_add_func("/varlink/listener-elogind-alias", test_listener_elogind_alias);
    g_test_add("/varlink/listener-active-socket", Fixture, NULL, setup,
               test_listener_active_socket, teardown);
    g_test_add("/varlink/listener-stale-socket", Fixture, NULL, setup,
               test_listener_stale_socket, teardown);
    g_test_add("/varlink/membership-nss-failures", Fixture, NULL, setup,
               test_membership_nss_failures, teardown);
    g_test_add("/varlink/slow-reply-nss-unlocked", Fixture, NULL, setup,
               test_slow_reply_does_not_hold_nss_lock, teardown);
    g_test_add("/varlink/nss-lock-contention", Fixture, NULL, setup,
               test_nss_lock_contention, teardown);
    g_test_add("/varlink/nss-provider-recursion", Fixture, NULL, setup,
               test_nss_provider_recursion, teardown);
    g_test_add("/varlink/unknown-method", Fixture, NULL, setup,
               test_unknown_method, teardown);
    g_test_add("/varlink/bad-service", Fixture, NULL, setup,
               test_bad_service, teardown);
    g_test_add("/varlink/invalid-json", Fixture, NULL, setup,
               test_invalid_json, teardown);
    g_test_add("/varlink/multiple-requests", Fixture, NULL, setup,
               test_multiple_requests, teardown);
    g_test_add("/varlink/pipelined-requests", Fixture, NULL, setup,
               test_pipelined_requests, teardown);
    g_test_add("/varlink/concurrent-requests", Fixture, NULL, setup,
               test_concurrent_requests, teardown);
    g_test_add("/varlink/worker-limit-recovery", Fixture, NULL, setup,
               test_worker_limit_recovery, teardown);
    g_test_add("/varlink/shutdown-saturated", Fixture, NULL, setup,
               test_shutdown_saturated, teardown);
    g_test_add("/varlink/repeated-saturation", Fixture, NULL, setup,
               test_repeated_saturation, teardown);
    return g_test_run();
}
