#include <gio/gio.h>
#include <syslog.h>
#include <unistd.h>

#include "rcl-home.h"
#include "rcl-varlink.h"

#define HOMED_BUS_NAME "org.freedesktop.home1"
#define HOMED_OBJECT_PATH "/org/freedesktop/home1"

static GMainLoop *main_loop;
static RclHome *home_service;
static GSocketService *varlink_service;
static guint name_owner_id;
static guint reconnect_source;

static void bus_acquired(GDBusConnection *connection,
                         const gchar *name,
                         gpointer data);
static void name_lost(GDBusConnection *connection,
                      const gchar *name,
                      gpointer data);

static gboolean
reconnect_to_bus(gpointer data)
{
    (void) data;

    reconnect_source = 0;
    name_owner_id = g_bus_own_name(G_BUS_TYPE_SYSTEM, HOMED_BUS_NAME,
        G_BUS_NAME_OWNER_FLAGS_NONE, bus_acquired, NULL, name_lost, NULL,
        NULL);

    return G_SOURCE_REMOVE;
}

static void
bus_acquired(GDBusConnection *connection, const gchar *name, gpointer data)
{
    GError *error = NULL;

    (void) name;
    (void) data;

    if (!rcl_home_register(home_service, connection, &error)) {
        syslog(LOG_ERR, "cannot register D-Bus objects: %s",
               error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        g_main_loop_quit(main_loop);
        return;
    }

    syslog(LOG_INFO, "homed D-Bus object registered at %s",
           HOMED_OBJECT_PATH);
}

static void
name_lost(GDBusConnection *connection, const gchar *name, gpointer data)
{
    (void) connection;
    (void) data;

    syslog(LOG_ERR, "D-Bus name lost: %s", name);

    if (name_owner_id != 0) {
        guint old_owner = name_owner_id;

        name_owner_id = 0;
        g_bus_unown_name(old_owner);
    }

    if (reconnect_source == 0) {
        syslog(LOG_INFO, "scheduling reconnect in 2500 ms");
        reconnect_source = g_timeout_add(2500, reconnect_to_bus, NULL);
    }
}

int
main(void)
{
    GError *error = NULL;

    openlog("homed", LOG_PID, LOG_DAEMON);

    if (geteuid() != 0) {
        syslog(LOG_ERR, "homed must run as root");
        closelog();
        return 1;
    }

    home_service = rcl_home_new();
    if (home_service == NULL) {
        syslog(LOG_ERR, "cannot initialize home1 service");
        closelog();
        return 1;
    }

    varlink_service = rcl_varlink_start(&error);
    if (varlink_service == NULL) {
        syslog(LOG_ERR, "cannot start Varlink listener: %s",
               error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        rcl_home_free(home_service);
        closelog();
        return 1;
    }

    main_loop = g_main_loop_new(NULL, FALSE);
    name_owner_id = g_bus_own_name(G_BUS_TYPE_SYSTEM, HOMED_BUS_NAME,
        G_BUS_NAME_OWNER_FLAGS_NONE, bus_acquired, NULL, name_lost, NULL,
        NULL);

    syslog(LOG_INFO, "homed started (0.3.1)");
    syslog(LOG_INFO, "homed Varlink listener registered at %s",
           "/run/systemd/userdb/io.systemd.Home");

    g_main_loop_run(main_loop);

    if (reconnect_source != 0)
        g_source_remove(reconnect_source);
    if (name_owner_id != 0)
        g_bus_unown_name(name_owner_id);

    rcl_varlink_stop(varlink_service);
    rcl_home_free(home_service);
    g_main_loop_unref(main_loop);
    closelog();

    return 0;
}
