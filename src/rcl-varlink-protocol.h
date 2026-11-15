#pragma once
#include <gio/gio.h>
/* Run the Varlink framing and request dispatcher on an accepted connection. */
void rcl_varlink_serve_connection(GSocketConnection *connection);
