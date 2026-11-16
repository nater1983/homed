#pragma once
#include <gio/gio.h>
typedef struct _RclVarlinkWorkers RclVarlinkWorkers;
RclVarlinkWorkers *rcl_varlink_workers_new(void);
void rcl_varlink_workers_free(gpointer data);
gboolean rcl_varlink_workers_accept(RclVarlinkWorkers *workers,
                                    GSocketConnection *connection);
void rcl_varlink_workers_shutdown(RclVarlinkWorkers *workers);
