#pragma once
#include <gio/gio.h>
GSocketService *rcl_varlink_server_start(const gchar *runtime_directory,
                                         GError **error);
GSocketService *rcl_varlink_server_start_at(const gchar *directory,
                                            const gchar *socket_path,
                                            GError **error);
void rcl_varlink_server_stop(GSocketService *service);
