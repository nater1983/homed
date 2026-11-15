#include "rcl-varlink-server.h"
#include "rcl-varlink-worker.h"
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <sys/stat.h>

static gboolean
incoming_connection(GSocketService *service, GSocketConnection *connection,
                    GObject *source_object, gpointer user_data)
{
    (void) service;
    (void) source_object;
    return rcl_varlink_workers_accept(user_data, connection);
}

GSocketService *
rcl_varlink_server_start_at(const gchar *directory, const gchar *socket_path,
                            GError **error)
{
    struct stat status;
    GSocketService *service;
    GSocketAddress *address;
    GSocketAddress *effective_address = NULL;
    RclVarlinkWorkers *workers;

    if (g_mkdir_with_parents(directory, 0755) < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot create %s: %s", directory, g_strerror(errno));
        return NULL;
    }
    if (lstat(socket_path, &status) == 0) {
        GSocket *probe;
        gboolean connected;
        if (!S_ISSOCK(status.st_mode)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "%s exists and is not a socket", socket_path);
            return NULL;
        }
        probe = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                             G_SOCKET_PROTOCOL_DEFAULT, error);
        if (probe == NULL)
            return NULL;
        address = g_unix_socket_address_new(socket_path);
        connected = g_socket_connect(probe, address, NULL, NULL);
        g_object_unref(address);
        g_object_unref(probe);
        if (connected) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_ADDRESS_IN_USE,
                        "another service is listening on %s", socket_path);
            return NULL;
        }
        if (g_unlink(socket_path) < 0) {
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                        "cannot remove stale socket %s: %s", socket_path,
                        g_strerror(errno));
            return NULL;
        }
    } else if (errno != ENOENT) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot inspect %s: %s", socket_path, g_strerror(errno));
        return NULL;
    }

    service = g_socket_service_new();
    address = g_unix_socket_address_new(socket_path);
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(service), address,
            G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT, NULL,
            &effective_address, error)) {
        g_object_unref(address);
        g_object_unref(service);
        return NULL;
    }
    g_object_unref(address);
    g_clear_object(&effective_address);
    if (g_chmod(socket_path, 0666) < 0) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno),
                    "cannot set permissions on %s: %s", socket_path,
                    g_strerror(errno));
        g_socket_listener_close(G_SOCKET_LISTENER(service));
        g_object_unref(service);
        g_unlink(socket_path);
        return NULL;
    }

    workers = rcl_varlink_workers_new();
    g_object_set_data_full(G_OBJECT(service), "homed-varlink-workers",
                           workers, rcl_varlink_workers_free);
    g_signal_connect(service, "incoming", G_CALLBACK(incoming_connection),
                     workers);
    g_object_set_data_full(G_OBJECT(service), "homed-varlink-socket-path",
                           g_strdup(socket_path), g_free);
    g_socket_service_start(service);
    return service;
}

void
rcl_varlink_server_stop(GSocketService *service)
{
    if (service == NULL)
        return;
    const gchar *socket_path =
        g_object_get_data(G_OBJECT(service), "homed-varlink-socket-path");
    RclVarlinkWorkers *workers =
        g_object_get_data(G_OBJECT(service), "homed-varlink-workers");
    g_socket_service_stop(service);
    g_socket_listener_close(G_SOCKET_LISTENER(service));
    rcl_varlink_workers_shutdown(workers);
    if (socket_path != NULL)
        g_unlink(socket_path);
    g_object_unref(service);
}
