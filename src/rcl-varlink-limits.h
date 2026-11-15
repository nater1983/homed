#pragma once

#include <gio/gio.h>
#include <sys/types.h>

/* Compatibility ceilings; threads have a separate, smaller cap. */
#define RCL_VARLINK_GLOBAL_LIMIT 4096U
#define RCL_VARLINK_PER_UID_LIMIT 128U
#define RCL_VARLINK_WORKER_LIMIT 128U

typedef struct _RclVarlinkLimits RclVarlinkLimits;

RclVarlinkLimits *rcl_varlink_limits_new(void);
void rcl_varlink_limits_free(RclVarlinkLimits *limits);

/* Admission and release are serialized by the limits object's mutex.
 * Returns FALSE if peer credentials are unavailable or a cap is reached. */
gboolean rcl_varlink_limits_acquire(RclVarlinkLimits *limits,
                                    GSocketConnection *connection,
                                    uid_t *uid_out);
void rcl_varlink_limits_release(RclVarlinkLimits *limits, uid_t uid);
