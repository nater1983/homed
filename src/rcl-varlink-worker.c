#include "rcl-varlink-worker.h"
#include "rcl-varlink-limits.h"
#include "rcl-varlink-protocol.h"

struct _RclVarlinkWorkers {
    GMutex mutex;
    GCond drained;
    GList *connections; /* Owned references; protected by mutex. */
    gboolean stopping;
    RclVarlinkLimits *limits;
};

typedef struct {
    RclVarlinkWorkers *workers;
    GSocketConnection *connection;
    uid_t uid;
} VarlinkWorker;

RclVarlinkWorkers *
rcl_varlink_workers_new(void)
{
    RclVarlinkWorkers *workers = g_new0(RclVarlinkWorkers, 1);
    g_mutex_init(&workers->mutex);
    g_cond_init(&workers->drained);
    workers->limits = rcl_varlink_limits_new();
    return workers;
}

void
rcl_varlink_workers_free(gpointer data)
{
    RclVarlinkWorkers *workers = data;
    if (workers == NULL)
        return;
    g_assert(workers->connections == NULL);
    rcl_varlink_limits_free(workers->limits);
    g_cond_clear(&workers->drained);
    g_mutex_clear(&workers->mutex);
    g_free(workers);
}

static gpointer
worker_main(gpointer data)
{
    VarlinkWorker *worker = data;
    RclVarlinkWorkers *workers = worker->workers;

    rcl_varlink_serve_connection(worker->connection);
    g_io_stream_close(G_IO_STREAM(worker->connection), NULL, NULL);

    g_mutex_lock(&workers->mutex);
    rcl_varlink_limits_release(workers->limits, worker->uid);
    workers->connections = g_list_remove(workers->connections,
                                          worker->connection);
    if (workers->connections == NULL)
        g_cond_signal(&workers->drained);
    g_mutex_unlock(&workers->mutex);

    g_object_unref(worker->connection);
    g_free(worker);
    return NULL;
}

gboolean
rcl_varlink_workers_accept(RclVarlinkWorkers *workers,
                           GSocketConnection *connection)
{
    VarlinkWorker *worker;
    GThread *thread;
    GError *error = NULL;
    uid_t uid;

    g_mutex_lock(&workers->mutex);
    if (workers->stopping ||
        !rcl_varlink_limits_acquire(workers->limits, connection, &uid)) {
        g_mutex_unlock(&workers->mutex);
        g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
        return TRUE; /* Explicitly consume rejected connections. */
    }

    worker = g_new0(VarlinkWorker, 1);
    worker->workers = workers;
    worker->uid = uid;
    worker->connection = g_object_ref(connection);
    workers->connections = g_list_prepend(workers->connections,
                                          worker->connection);
    thread = g_thread_try_new("homed-varlink", worker_main, worker, &error);
    if (thread == NULL) {
        workers->connections = g_list_remove(workers->connections,
                                              worker->connection);
        rcl_varlink_limits_release(workers->limits, uid);
        g_object_unref(worker->connection);
        g_free(worker);
        g_warning("Cannot start Varlink worker: %s",
                  error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        g_mutex_unlock(&workers->mutex);
        g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
        return TRUE;
    }
    g_thread_unref(thread);
    g_mutex_unlock(&workers->mutex);
    return TRUE;
}

void
rcl_varlink_workers_shutdown(RclVarlinkWorkers *workers)
{
    g_mutex_lock(&workers->mutex);
    workers->stopping = TRUE;
    for (GList *item = workers->connections; item != NULL;
         item = item->next) {
        GSocket *socket = g_socket_connection_get_socket(item->data);
        g_socket_shutdown(socket, TRUE, TRUE, NULL);
    }
    while (workers->connections != NULL)
        g_cond_wait(&workers->drained, &workers->mutex);
    g_mutex_unlock(&workers->mutex);
}
