#include "rcl-varlink-limits.h"

#include <gio/gio.h>

struct _RclVarlinkLimits {
    GMutex mutex;
    GHashTable *per_uid;
    guint active;
};

RclVarlinkLimits *
rcl_varlink_limits_new(void)
{
    RclVarlinkLimits *limits = g_new0(RclVarlinkLimits, 1);
    g_mutex_init(&limits->mutex);
    limits->per_uid = g_hash_table_new(g_direct_hash, g_direct_equal);
    return limits;
}

void
rcl_varlink_limits_free(RclVarlinkLimits *limits)
{
    if (limits == NULL)
        return;
    g_mutex_lock(&limits->mutex);
    g_assert(limits->active == 0);
    g_assert(g_hash_table_size(limits->per_uid) == 0);
    g_mutex_unlock(&limits->mutex);
    g_hash_table_unref(limits->per_uid);
    g_mutex_clear(&limits->mutex);
    g_free(limits);
}

gboolean
rcl_varlink_limits_acquire(RclVarlinkLimits *limits,
                           GSocketConnection *connection, uid_t *uid_out)
{
    GSocket *socket;
    GCredentials *credentials;
    GError *error = NULL;
    uid_t uid;
    gpointer key;
    guint count;
    gboolean accepted = FALSE;

    g_return_val_if_fail(limits != NULL, FALSE);
    g_return_val_if_fail(G_IS_SOCKET_CONNECTION(connection), FALSE);

    socket = g_socket_connection_get_socket(connection);
    credentials = g_socket_get_credentials(socket, &error);
    if (credentials == NULL) {
        g_clear_error(&error);
        return FALSE;
    }

    uid = g_credentials_get_unix_user(credentials, &error);
    g_object_unref(credentials);
    if (error != NULL || uid == (uid_t) -1) {
        g_clear_error(&error);
        return FALSE;
    }

    /* UID 0 must be representable, so store counters as count+1. */
    key = GUINT_TO_POINTER((guint) uid);
    g_mutex_lock(&limits->mutex);
    count = GPOINTER_TO_UINT(g_hash_table_lookup(limits->per_uid, key));
    if (limits->active < RCL_VARLINK_GLOBAL_LIMIT &&
        limits->active < RCL_VARLINK_WORKER_LIMIT &&
        count < RCL_VARLINK_PER_UID_LIMIT) {
        g_hash_table_insert(limits->per_uid, key, GUINT_TO_POINTER(count + 1));
        limits->active++;
        accepted = TRUE;
        if (uid_out != NULL)
            *uid_out = uid;
    }
    g_mutex_unlock(&limits->mutex);
    return accepted;
}

void
rcl_varlink_limits_release(RclVarlinkLimits *limits, uid_t uid)
{
    gpointer key;
    guint count;

    g_return_if_fail(limits != NULL);
    key = GUINT_TO_POINTER((guint) uid);
    g_mutex_lock(&limits->mutex);
    count = GPOINTER_TO_UINT(g_hash_table_lookup(limits->per_uid, key));
    g_assert(count > 0 && limits->active > 0);
    if (count == 1)
        g_hash_table_remove(limits->per_uid, key);
    else
        g_hash_table_insert(limits->per_uid, key, GUINT_TO_POINTER(count - 1));
    limits->active--;
    g_mutex_unlock(&limits->mutex);
}
