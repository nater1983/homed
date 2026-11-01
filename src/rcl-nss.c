#include "rcl-nss.h"

static GMutex nss_mutex;

void
rcl_nss_lock(void)
{
    g_mutex_lock(&nss_mutex);
}

void
rcl_nss_unlock(void)
{
    g_mutex_unlock(&nss_mutex);
}
