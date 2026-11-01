#pragma once

#include <glib.h>

/* Serialize NSS access shared by the D-Bus and Varlink interfaces. */
void rcl_nss_lock(void);
void rcl_nss_unlock(void);
