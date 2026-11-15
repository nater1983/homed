#pragma once

#include <glib.h>
#include <json-glib/json-glib.h>
#include <pwd.h>

/* Serialize NSS access shared by the D-Bus and Varlink interfaces. */
void rcl_nss_lock(void);
gboolean rcl_nss_lock_timeout(guint timeout_ms);
#ifdef HOMED_TESTING
typedef void (*RclNssTestCallback)(void);
void rcl_nss_test_set_callback(RclNssTestCallback callback);
#endif
void rcl_nss_unlock(void);

const gchar *rcl_nss_user_shell(const struct passwd *user);
JsonObject *rcl_nss_user_record(const struct passwd *user, const gchar *service);
