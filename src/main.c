#include <gio/gio.h>
#include <syslog.h>
#include <unistd.h>
#include "rcl-home.h"
#define HOMED_BUS_NAME "org.freedesktop.home1"
#define HOMED_OBJECT_PATH "/org/freedesktop/home1"
static GMainLoop *main_loop;
static RclHome *home_service;
static void bus_acquired(GDBusConnection *connection,const gchar *name,gpointer data) {
    GError *error=NULL; (void)data;
    if (!rcl_home_register(home_service,connection,&error)) {
        syslog(LOG_ERR,"cannot register D-Bus objects: %s",error?error->message:"unknown error");
        g_clear_error(&error); g_main_loop_quit(main_loop); return;
    }
    syslog(LOG_INFO,"homed registered %s at %s",name,HOMED_OBJECT_PATH);
}
static void name_lost(GDBusConnection *connection,const gchar *name,gpointer data) {
    (void)connection;(void)data; syslog(LOG_ERR,"lost or could not acquire D-Bus name %s",name);
}
int main(void) {
    guint owner; openlog("homed",LOG_PID,LOG_DAEMON);
    if (geteuid()!=0) { syslog(LOG_ERR,"homed must run as root"); closelog(); return 1; }
    home_service=rcl_home_new();
    if (!home_service) { syslog(LOG_ERR,"cannot initialize home1 service"); closelog(); return 1; }
    main_loop=g_main_loop_new(NULL,FALSE);
    owner=g_bus_own_name(G_BUS_TYPE_SYSTEM,HOMED_BUS_NAME,G_BUS_NAME_OWNER_FLAGS_NONE,bus_acquired,NULL,name_lost,NULL,NULL);
    syslog(LOG_INFO,"homed started (0.2.0)"); g_main_loop_run(main_loop);
    g_bus_unown_name(owner); rcl_home_free(home_service); g_main_loop_unref(main_loop); closelog(); return 0;
}
