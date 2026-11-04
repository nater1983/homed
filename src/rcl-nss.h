#pragma once

#include <glib.h>

/* Serialize NSS access shared by the D-Bus and Varlink interfaces. */
void rcl_nss_lock(void);
gboolean rcl_nss_lock_timeout(guint timeout_ms);
#ifdef HOMED_TESTING
typedef void (*RclNssTestCallback)(void);
void rcl_nss_test_set_callback(RclNssTestCallback callback);
#endif
void rcl_nss_unlock(void);
