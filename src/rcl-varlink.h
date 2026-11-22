#pragma once

#include <gio/gio.h>

GSocketService *rcl_varlink_start(GError **error);
GSocketService *rcl_varlink_start_at(const gchar *directory,
                                    const gchar *socket_path,
                                    GError **error);
void rcl_varlink_stop(GSocketService *service);

#ifdef HOMED_TESTING
/* Simulate supplementary-group provider failure in integration tests. */
void rcl_varlink_test_fail_group_list(gboolean enabled);
/* Pause immediately before writing an NSS reply, after releasing the lock. */
void rcl_varlink_test_set_before_nss_reply_write(void (*callback)(void));
#endif
