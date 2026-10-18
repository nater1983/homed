#pragma once
#include <gio/gio.h>

typedef struct _RclHome RclHome;
RclHome *rcl_home_new(void);
void rcl_home_free(RclHome *self);
gboolean rcl_home_register(RclHome *self, GDBusConnection *connection, GError **error);
