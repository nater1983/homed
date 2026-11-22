#include "rcl-varlink-server.h"
#include "rcl-varlink-worker.h"
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

static gboolean
check_socket_directory(const gchar *directory, gboolean final, GError **error)
{
    struct stat status;
    g_autofree gchar *parent = g_path_get_dirname(directory);
    int result;

    if (!g_str_equal(parent, directory) &&
        !check_socket_directory(parent, FALSE, error))
        return FALSE;

    result = lstat(directory, &status);
    if (result < 0 && errno == ENOENT) {
        if (g_mkdir(directory, 0755) < 0 && errno != EEXIST) {
            int saved_errno = errno;
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                        "cannot create %s: %s", directory, g_strerror(saved_errno));
            return FALSE;
        }
        result = lstat(directory, &status);
    }
    if (result < 0) {
        int saved_errno = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                    "cannot inspect %s: %s", directory, g_strerror(saved_errno));
        return FALSE;
    }

    /* A trusted sticky ancestor such as /tmp protects entries owned by
     * the daemon. The directory containing the socket must be private
     * to its owner for writes. */
    if (!S_ISDIR(status.st_mode) ||
        (status.st_uid != 0 && status.st_uid != geteuid()) ||
        ((status.st_mode & (S_IWGRP | S_IWOTH)) &&
         (final || !(status.st_mode & S_ISVTX)))) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "unsafe Varlink socket directory: %s", directory);
        return FALSE;
    }
    return TRUE;
}

/* Never remove an unrelated entry if the socket pathname was replaced. */
static void
unlink_matching_socket(const gchar *path, const struct stat *identity)
{
    struct stat current;
    if (path != NULL && identity != NULL &&
        lstat(path, &current) == 0 &&
        S_ISSOCK(current.st_mode) &&
        current.st_uid == identity->st_uid &&
        current.st_dev == identity->st_dev &&
        current.st_ino == identity->st_ino)
        g_unlink(path);
}

static gboolean
incoming_connection(GSocketService *service, GSocketConnection *connection,
                    GObject *source_object, gpointer user_data)
{
    (void) service;
    (void) source_object;
    return rcl_varlink_workers_accept(user_data, connection);
}

GSocketService *
rcl_varlink_server_start(const gchar *runtime_directory, GError **error)
{
    if (!check_socket_directory(runtime_directory, FALSE, error))
        return NULL;

    g_autofree gchar *systemd = g_build_filename(runtime_directory, "systemd", NULL);
    const gchar *provider_directory = systemd;
    g_autofree gchar *elogind = NULL;
    struct stat status;

    /* Slackware exposes elogind's runtime directory through this alias. */
    if (lstat(systemd, &status) == 0 && S_ISLNK(status.st_mode)) {
        g_autofree gchar *target = g_file_read_link(systemd, error);
        if (target == NULL)
            return NULL;

        elogind = g_build_filename(runtime_directory, "elogind", NULL);
        if (status.st_uid != geteuid() ||
            (!g_str_equal(target, "elogind") && !g_str_equal(target, elogind))) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "unsafe Varlink socket directory: %s", systemd);
            return NULL;
        }
        provider_directory = elogind;
    }

    g_autofree gchar *directory = g_build_filename(provider_directory, "userdb", NULL);
    g_autofree gchar *socket_path = g_build_filename(directory, "io.systemd.Home", NULL);
    return rcl_varlink_server_start_at(directory, socket_path, error);
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
    g_autofree gchar *socket_directory = g_path_get_dirname(socket_path);

    if (!g_path_is_absolute(directory) ||
        !g_str_equal(directory, socket_directory)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Varlink socket must be inside its specified absolute directory");
        return NULL;
    }
    if (!check_socket_directory(directory, TRUE, error))
        return NULL;

    if (lstat(socket_path, &status) == 0) {
        g_autoptr(GError) probe_error = NULL;
        GSocket *probe;
        gboolean connected;
        if (!S_ISSOCK(status.st_mode)) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "%s exists and is not a socket", socket_path);
            return NULL;
        }
        if (status.st_uid != geteuid()) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "cannot replace socket owned by another user: %s", socket_path);
            return NULL;
        }
        probe = g_socket_new(G_SOCKET_FAMILY_UNIX, G_SOCKET_TYPE_STREAM,
                             G_SOCKET_PROTOCOL_DEFAULT, error);
        if (probe == NULL)
            return NULL;
        g_socket_set_blocking(probe, FALSE);
        address = g_unix_socket_address_new(socket_path);
        connected = g_socket_connect(probe, address, NULL, &probe_error);
        g_object_unref(address);
        g_object_unref(probe);
        if (connected) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_ADDRESS_IN_USE,
                        "another service is listening on %s", socket_path);
            return NULL;
        }
        if (!g_error_matches(probe_error, G_IO_ERROR,
                             G_IO_ERROR_CONNECTION_REFUSED)) {
            g_propagate_prefixed_error(error, g_steal_pointer(&probe_error),
                                       "cannot probe %s: ", socket_path);
            return NULL;
        }
        /* A stale socket may have been replaced while the connection
         * probe ran. Refuse cleanup unless it is still the same inode. */
        struct stat current;
        if (lstat(socket_path, &current) != 0 ||
            !S_ISSOCK(current.st_mode) ||
            current.st_uid != status.st_uid ||
            current.st_dev != status.st_dev ||
            current.st_ino != status.st_ino) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS,
                        "stale Varlink socket changed during probe: %s", socket_path);
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

    /* Record the bound inode before changing pathname permissions. */
    struct stat bound_socket;
    if (lstat(socket_path, &bound_socket) != 0 ||
        !S_ISSOCK(bound_socket.st_mode) ||
        bound_socket.st_uid != geteuid()) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "cannot verify bound Varlink socket");
        g_socket_listener_close(G_SOCKET_LISTENER(service));
        g_object_unref(service);
        return NULL;
    }
    if (g_chmod(socket_path, 0666) < 0) {
        int saved_errno = errno;
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno),
                    "cannot set permissions on %s: %s", socket_path,
                    g_strerror(saved_errno));
        g_socket_listener_close(G_SOCKET_LISTENER(service));
        unlink_matching_socket(socket_path, &bound_socket);
        g_object_unref(service);
        return NULL;
    }
    /* Refuse to serve if another inode replaced the path during setup. */
    struct stat current_socket;
    if (lstat(socket_path, &current_socket) != 0 ||
        !S_ISSOCK(current_socket.st_mode) ||
        current_socket.st_uid != bound_socket.st_uid ||
        current_socket.st_dev != bound_socket.st_dev ||
        current_socket.st_ino != bound_socket.st_ino) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Varlink socket changed during setup");
        g_socket_listener_close(G_SOCKET_LISTENER(service));
        g_object_unref(service);
        return NULL;
    }

    workers = rcl_varlink_workers_new();
    g_object_set_data_full(G_OBJECT(service), "homed-varlink-workers",
                           workers, rcl_varlink_workers_free);
    g_signal_connect(service, "incoming", G_CALLBACK(incoming_connection),
                     workers);
    g_object_set_data_full(G_OBJECT(service), "homed-varlink-socket-path",
                           g_strdup(socket_path), g_free);
    struct stat *socket_identity = g_new(struct stat, 1);
    *socket_identity = bound_socket;
    g_object_set_data_full(G_OBJECT(service), "homed-varlink-socket-identity",
                           socket_identity, g_free);
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
    /* Remove the path while the listener still owns it. A new listener
     * can bind the path while the old workers are draining. */
    const struct stat *identity =
        g_object_get_data(G_OBJECT(service), "homed-varlink-socket-identity");
    unlink_matching_socket(socket_path, identity);
    g_socket_listener_close(G_SOCKET_LISTENER(service));
    rcl_varlink_workers_shutdown(workers);
    g_object_unref(service);
}
