#pragma once

#include <gio/gio.h>

GSocketService *rcl_varlink_start(GError **error);
void rcl_varlink_stop(GSocketService *service);
