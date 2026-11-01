#pragma once

#include <gio/gio.h>

GSocketService *rcl_varlink_start(GError **error);
GSocketService *rcl_varlink_start_at(const gchar *directory,
                                    const gchar *socket_path,
                                    GError **error);
void rcl_varlink_stop(GSocketService *service);
