#include "rcl-varlink.h"
#include "rcl-varlink-limits.h"
#include "rcl-varlink-worker.h"

#include <json-glib/json-glib.h>
#include <string.h>
#include <sys/socket.h>

static GSocketConnection *
connection_from_fd(int fd)
{
    g_autoptr(GError) error = NULL;
    g_autoptr(GSocket) socket = g_socket_new_from_fd(fd, &error);

    g_assert_no_error(error);
    g_assert_nonnull(socket);
    return g_socket_connection_factory_create_connection(socket);
}

static GSocketConnection *
connect_worker(RclVarlinkWorkers *workers, gboolean *accepted)
{
    int sockets[2];

    g_assert_cmpint(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets), ==, 0);
    g_autoptr(GSocketConnection) server = connection_from_fd(sockets[0]);
    GSocketConnection *client = connection_from_fd(sockets[1]);
    g_socket_set_timeout(g_socket_connection_get_socket(client), 5);

    g_assert_true(rcl_varlink_workers_accept(workers, server));
    *accepted = !g_io_stream_is_closed(G_IO_STREAM(server));
    return client;
}

static void
assert_closed(GSocketConnection *connection)
{
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    g_autoptr(GError) error = NULL;
    gchar byte;

    g_assert_cmpint(g_input_stream_read(input, &byte, 1, NULL, &error), ==, 0);
    g_assert_no_error(error);
}

static void
test_saturated_shutdown(void)
{
    RclVarlinkWorkers *workers = rcl_varlink_workers_new();
    g_autoptr(GPtrArray) clients = g_ptr_array_new_with_free_func(g_object_unref);
    gboolean accepted;

    for (guint i = 0; i < RCL_VARLINK_WORKER_LIMIT; i++) {
        GSocketConnection *client = connect_worker(workers, &accepted);
        g_assert_true(accepted);
        g_ptr_array_add(clients, client);
    }

    for (guint round = 0; round < 3; round++) {
        g_autoptr(GSocketConnection) rejected = connect_worker(workers, &accepted);
        g_assert_false(accepted);
        assert_closed(rejected);

        g_ptr_array_remove_index_fast(clients, clients->len - 1);
        gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
        do {
            GSocketConnection *client = connect_worker(workers, &accepted);
            if (accepted) {
                g_ptr_array_add(clients, client);
                break;
            }
            g_object_unref(client);
            g_usleep(1000);
        } while (g_get_monotonic_time() < deadline);
        g_assert_true(accepted);
    }

    g_assert_cmpuint(clients->len, ==, RCL_VARLINK_WORKER_LIMIT);
    rcl_varlink_workers_shutdown(workers);
    for (guint i = 0; i < clients->len; i++)
        assert_closed(g_ptr_array_index(clients, i));

    g_autoptr(GSocketConnection) after_shutdown = connect_worker(workers, &accepted);
    g_assert_false(accepted);
    assert_closed(after_shutdown);
    rcl_varlink_workers_free(workers);
}

static JsonNode *
get_membership_reply(GSocketConnection *client, const gchar *filter)
{
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(client));
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(client));
    g_autoptr(GError) error = NULL;
    g_autofree gchar *request = g_strdup_printf(
        "{\"method\":\"io.systemd.UserDatabase.GetMemberships\","
        "\"parameters\":{\"service\":\"io.systemd.Home\",%s}}", filter);
    g_autoptr(GString) reply = g_string_new(NULL);
    g_autoptr(JsonParser) parser = json_parser_new();

    g_assert_true(g_output_stream_write_all(output, request, strlen(request) + 1,
                                            NULL, NULL, &error));
    g_assert_no_error(error);

    for (;;) {
        gchar byte;
        g_assert_cmpint(g_input_stream_read(input, &byte, 1, NULL, &error), ==, 1);
        g_assert_no_error(error);
        if (byte == '\0')
            break;
        g_string_append_c(reply, byte);
    }

    g_assert_true(json_parser_load_from_data(parser, reply->str, reply->len, &error));
    g_assert_no_error(error);
    return json_node_copy(json_parser_get_root(parser));
}

static void
test_membership_failure(void)
{
    const gchar *filters[] = {
        "\"userName\":\"root\"",
        "\"groupName\":\"root\"",
        "\"userName\":\"root\",\"groupName\":\"root\"",
    };
    RclVarlinkWorkers *workers = rcl_varlink_workers_new();
    gboolean accepted;
    g_autoptr(GSocketConnection) client = connect_worker(workers, &accepted);

    g_assert_true(accepted);
    for (guint i = 0; i < G_N_ELEMENTS(filters); i++) {
        rcl_varlink_test_fail_group_list(TRUE);
        g_autoptr(JsonNode) failed_reply = get_membership_reply(client, filters[i]);
        JsonObject *failed = json_node_get_object(failed_reply);
        g_assert_cmpstr(json_object_get_string_member(failed, "error"), ==,
                        "io.systemd.UserDatabase.ServiceNotAvailable");

        rcl_varlink_test_fail_group_list(FALSE);
        g_autoptr(JsonNode) reply = get_membership_reply(client, filters[i]);
        JsonObject *object = json_node_get_object(reply);
        g_assert_true(json_object_has_member(object, "parameters"));
        g_assert_false(json_object_get_boolean_member(object, "continues"));
    }

    rcl_varlink_workers_shutdown(workers);
    rcl_varlink_workers_free(workers);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/varlink/saturation-recovery-shutdown", test_saturated_shutdown);
    g_test_add_func("/varlink/membership-failure-recovery", test_membership_failure);
    return g_test_run();
}
