#pragma once

#include <glib.h>

/* Serialize NSS access shared by the D-Bus and Varlink interfaces. */
void rcl_nss_lock(void);
gboolean rcl_nss_lock_timeout(guint timeout_ms);
void rcl_nss_unlock(void);
