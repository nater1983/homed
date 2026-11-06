#include "rcl-nss.h"

static GMutex nss_mutex;

void
rcl_nss_lock(void)
{
    g_mutex_lock(&nss_mutex);
}

gboolean
rcl_nss_lock_timeout(guint timeout_ms)
{
    gint64 deadline = g_get_monotonic_time() + ((gint64) timeout_ms * 1000);
    do {
        if (g_mutex_trylock(&nss_mutex))
            return TRUE;
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    return g_mutex_trylock(&nss_mutex);
}

void
rcl_nss_unlock(void)
{
    g_mutex_unlock(&nss_mutex);
}
