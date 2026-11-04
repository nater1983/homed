#include "rcl-nss.h"

static GMutex nss_mutex;

#ifdef HOMED_TESTING
static RclNssTestCallback test_callback;

void
rcl_nss_test_set_callback(RclNssTestCallback callback)
{
    test_callback = callback;
}
#endif

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
        if (g_mutex_trylock(&nss_mutex)) {
#ifdef HOMED_TESTING
            if (test_callback != NULL) {
                RclNssTestCallback callback = test_callback;
                test_callback = NULL;
                callback();
            }
#endif
            return TRUE;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);

    return g_mutex_trylock(&nss_mutex);
}

void
rcl_nss_unlock(void)
{
    g_mutex_unlock(&nss_mutex);
}
