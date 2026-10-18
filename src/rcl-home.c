#include "rcl-home.h"
#include <pwd.h>
#include <stdio.h>
#include <unistd.h>

#define OBJECT_PATH "/org/freedesktop/home1"

struct _RclHome {
    GDBusNodeInfo *node;
    GDBusConnection *connection;
    guint registration_id;
    GPtrArray *homes;
    GHashTable *homes_by_uid;
};
typedef struct {
    uid_t uid; gid_t gid;
    gchar *name, *real_name, *home_directory, *shell, *path;
    guint registration_id;
} HomeEntry;

static void home_method_call(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *, GVariant *, GDBusMethodInvocation *, gpointer);
static GVariant *home_get_property(GDBusConnection *, const gchar *, const gchar *, const gchar *, const gchar *, GError **, gpointer);
static const gchar xml[] =
"<node>"
"<interface name='org.freedesktop.home1.Manager'>"
"<method name='GetUserRecordByUID'><arg name='uid' type='u' direction='in'/><arg name='record' type='a{sv}' direction='out'/><arg name='incomplete' type='b' direction='out'/></method>"
"<method name='GetUserRecordByName'><arg name='user_name' type='s' direction='in'/><arg name='record' type='a{sv}' direction='out'/><arg name='incomplete' type='b' direction='out'/></method>"
"<method name='GetUserRecordByPID'><arg name='pid' type='u' direction='in'/><arg name='record' type='a{sv}' direction='out'/><arg name='incomplete' type='b' direction='out'/></method>"
"<method name='GetHomeByName'><arg name='user_name' type='s' direction='in'/><arg name='uid' type='u' direction='out'/><arg name='home_state' type='s' direction='out'/><arg name='gid' type='u' direction='out'/><arg name='real_name' type='s' direction='out'/><arg name='home_directory' type='s' direction='out'/><arg name='shell' type='s' direction='out'/><arg name='bus_path' type='o' direction='out'/></method>"
"<method name='GetHomeByUID'><arg name='uid' type='u' direction='in'/><arg name='user_name' type='s' direction='out'/><arg name='home_state' type='s' direction='out'/><arg name='gid' type='u' direction='out'/><arg name='real_name' type='s' direction='out'/><arg name='home_directory' type='s' direction='out'/><arg name='shell' type='s' direction='out'/><arg name='bus_path' type='o' direction='out'/></method>"
"<method name='ListHomes'><arg name='home_areas' type='a(susussso)' direction='out'/></method>"
"<property name='AutoLogin' type='a(sso)' access='read'/>"
"</interface><interface name='org.freedesktop.home1.Home'>"
"<method name='Activate'/><method name='Deactivate'/><method name='Lock'/><method name='Unlock'/>"
"<method name='Resize'><arg name='disk_size' type='t' direction='in'/></method>"
"<property name='UserName' type='s' access='read'/><property name='UID' type='u' access='read'/><property name='GID' type='u' access='read'/>"
"<property name='RealName' type='s' access='read'/><property name='HomeDirectory' type='s' access='read'/><property name='Shell' type='s' access='read'/>"
"<property name='State' type='s' access='read'/><property name='Service' type='s' access='read'/>"
"<signal name='Changed'/><signal name='Removed'/></interface></node>";

static gchar *home_object_path(uid_t uid) { return g_strdup_printf("/org/freedesktop/home1/home/_%u", (guint) uid); }
static const gchar *pw_shell(const struct passwd *pw) { return (pw->pw_shell && *pw->pw_shell) ? pw->pw_shell : "/bin/sh"; }
static void home_entry_free(gpointer data) {
    HomeEntry *h = data; if (!h) return;
    g_free(h->name); g_free(h->real_name); g_free(h->home_directory); g_free(h->shell); g_free(h->path); g_free(h);
}
static const GDBusInterfaceVTable home_vtable = { .method_call = home_method_call, .get_property = home_get_property };

static gboolean register_home_entry(RclHome *self, const struct passwd *pw, GError **error) {
    gpointer key = GUINT_TO_POINTER((guint) pw->pw_uid);
    if (g_hash_table_contains(self->homes_by_uid, key)) return TRUE;
    HomeEntry *h = g_new0(HomeEntry, 1);
    h->uid = pw->pw_uid; h->gid = pw->pw_gid;
    h->name = g_strdup(pw->pw_name ? pw->pw_name : "");
    h->real_name = g_strdup(pw->pw_gecos ? pw->pw_gecos : "");
    h->home_directory = g_strdup(pw->pw_dir ? pw->pw_dir : "");
    h->shell = g_strdup(pw_shell(pw)); h->path = home_object_path(pw->pw_uid);
    h->registration_id = g_dbus_connection_register_object(self->connection, h->path,
        self->node->interfaces[1], &home_vtable, h, NULL, error);
    if (!h->registration_id) { home_entry_free(h); return FALSE; }
    g_ptr_array_add(self->homes, h); g_hash_table_insert(self->homes_by_uid, key, h);
    return TRUE;
}

static void return_home(GDBusMethodInvocation *inv, const struct passwd *pw, gboolean by_name) {
    gchar *path = home_object_path(pw->pw_uid);
    const gchar *gecos = pw->pw_gecos ? pw->pw_gecos : "";
    if (by_name)
        g_dbus_method_invocation_return_value(inv, g_variant_new("(usussso)",
            (guint32)pw->pw_uid, "inactive", (guint32)pw->pw_gid, gecos,
            pw->pw_dir ? pw->pw_dir : "", pw_shell(pw), path));
    else
        g_dbus_method_invocation_return_value(inv, g_variant_new("(ssussso)",
            pw->pw_name ? pw->pw_name : "", "inactive", (guint32)pw->pw_gid,
            gecos, pw->pw_dir ? pw->pw_dir : "", pw_shell(pw), path));
    g_free(path);
}

static void return_user_record(GDBusMethodInvocation *inv, const struct passwd *pw) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "userName", g_variant_new_string(pw->pw_name ? pw->pw_name : ""));
    g_variant_builder_add(&b, "{sv}", "uid", g_variant_new_uint32((guint32)pw->pw_uid));
    g_variant_builder_add(&b, "{sv}", "gid", g_variant_new_uint32((guint32)pw->pw_gid));
    g_variant_builder_add(&b, "{sv}", "realName", g_variant_new_string(pw->pw_gecos ? pw->pw_gecos : ""));
    g_variant_builder_add(&b, "{sv}", "homeDirectory", g_variant_new_string(pw->pw_dir ? pw->pw_dir : ""));
    g_variant_builder_add(&b, "{sv}", "shell", g_variant_new_string(pw_shell(pw)));
    g_variant_builder_add(&b, "{sv}", "service", g_variant_new_string("nss"));
    g_variant_builder_add(&b, "{sv}", "disposition", g_variant_new_string("regular"));
    /* NSS has no managed-storage, binding, secret, or signature metadata. */
    g_dbus_method_invocation_return_value(inv, g_variant_new("(a{sv}b)", &b, TRUE));
}

static struct passwd *passwd_for_pid(pid_t pid) {
    gchar *file, *contents = NULL; gchar **lines; uid_t uid = (uid_t)-1;
    gboolean found = FALSE; struct passwd *pw = NULL;
    if (pid <= 0) return NULL;
    file = g_strdup_printf("/proc/%ld/status", (long)pid);
    if (!g_file_get_contents(file, &contents, NULL, NULL)) { g_free(file); return NULL; }
    g_free(file); lines = g_strsplit(contents, "\n", -1);
    for (guint i=0; lines[i]; i++) {
        unsigned long real_uid;
        if (sscanf(lines[i], "Uid:%lu", &real_uid) == 1) { uid=(uid_t)real_uid; found=TRUE; break; }
    }
    g_strfreev(lines); g_free(contents);
    if (found) pw = getpwuid(uid);
    return pw;
}

static void method_call(GDBusConnection *conn, const gchar *sender, const gchar *obj,
 const gchar *iface, const gchar *method, GVariant *params, GDBusMethodInvocation *inv, gpointer data) {
    RclHome *self = data; struct passwd *pw = NULL;
    (void)conn; (void)sender; (void)obj; (void)iface;
    if (g_str_equal(method, "GetUserRecordByUID")) {
        guint uid; g_variant_get(params,"(u)",&uid); pw=getpwuid((uid_t)uid);
        if (pw) { return_user_record(inv,pw); return; } goto no_user;
    }
    if (g_str_equal(method, "GetUserRecordByName")) {
        const gchar *name; g_variant_get(params,"(&s)",&name); pw=getpwnam(name);
        if (pw) { return_user_record(inv,pw); return; } goto no_user;
    }
    if (g_str_equal(method, "GetUserRecordByPID")) {
        guint pid; g_variant_get(params,"(u)",&pid); pw=passwd_for_pid((pid_t)pid);
        if (pw) { return_user_record(inv,pw); return; } goto no_user;
    }
    if (g_str_equal(method, "GetHomeByName")) {
        const gchar *name; GError *e=NULL; g_variant_get(params,"(&s)",&name); pw=getpwnam(name);
        if (!pw) goto no_home;
        if (!register_home_entry(self,pw,&e)) { g_dbus_method_invocation_return_gerror(inv,e); g_clear_error(&e); return; }
        return_home(inv,pw,TRUE); return;
    }
    if (g_str_equal(method, "GetHomeByUID")) {
        guint uid; GError *e=NULL; g_variant_get(params,"(u)",&uid); pw=getpwuid((uid_t)uid);
        if (!pw) goto no_home;
        if (!register_home_entry(self,pw,&e)) { g_dbus_method_invocation_return_gerror(inv,e); g_clear_error(&e); return; }
        return_home(inv,pw,FALSE); return;
    }
    if (g_str_equal(method, "ListHomes")) {
        GVariantBuilder b; GError *e=NULL; g_variant_builder_init(&b,G_VARIANT_TYPE("a(susussso)"));
        setpwent();
        while ((pw=getpwent()) != NULL) {
            gchar *path=home_object_path(pw->pw_uid);
            if (!register_home_entry(self,pw,&e)) {
                g_free(path); endpwent(); g_variant_builder_clear(&b);
                g_dbus_method_invocation_return_gerror(inv,e); g_clear_error(&e); return;
            }
            g_variant_builder_add(&b,"(susussso)",pw->pw_name ? pw->pw_name : "",
                (guint32)pw->pw_uid,"inactive",(guint32)pw->pw_gid,pw->pw_gecos ? pw->pw_gecos : "",
                pw->pw_dir ? pw->pw_dir : "",pw_shell(pw),path);
            g_free(path);
        }
        endpwent(); g_dbus_method_invocation_return_value(inv,g_variant_new("(a(susussso))",&b)); return;
    }
    g_dbus_method_invocation_return_dbus_error(inv,"org.freedesktop.DBus.Error.UnknownMethod","Unknown Manager method"); return;
no_user:
    g_dbus_method_invocation_return_dbus_error(inv,"org.freedesktop.home1.NoSuchUser","No NSS account exists for the requested identity"); return;
no_home:
    g_dbus_method_invocation_return_dbus_error(inv,"org.freedesktop.home1.NoSuchHome","No NSS account exists for the requested identity");
}

static GVariant *get_property(GDBusConnection *c,const gchar *s,const gchar *o,const gchar *i,const gchar *p,GError **e,gpointer d) {
    (void)c;(void)s;(void)o;(void)i;(void)e;(void)d;
    if (g_str_equal(p,"AutoLogin")) return g_variant_new_array(G_VARIANT_TYPE("(sso)"),NULL,0);
    return NULL;
}
static void home_method_call(GDBusConnection *c,const gchar *s,const gchar *o,const gchar *i,const gchar *m,GVariant *p,GDBusMethodInvocation *inv,gpointer d) {
    (void)c;(void)s;(void)o;(void)i;(void)p;(void)d;
    if (g_str_equal(m,"Activate")||g_str_equal(m,"Deactivate")||g_str_equal(m,"Lock")||g_str_equal(m,"Unlock")||g_str_equal(m,"Resize"))
        g_dbus_method_invocation_return_dbus_error(inv,"org.freedesktop.home1.NotSupported","NSS-backed home objects do not have managed storage");
    else g_dbus_method_invocation_return_dbus_error(inv,"org.freedesktop.DBus.Error.UnknownMethod","Unknown Home method");
}
static GVariant *home_get_property(GDBusConnection *c,const gchar *s,const gchar *o,const gchar *i,const gchar *p,GError **e,gpointer d) {
    HomeEntry *h=d; (void)c;(void)s;(void)o;(void)i;(void)e;
    if (g_str_equal(p,"UserName")) return g_variant_new_string(h->name);
    if (g_str_equal(p,"UID")) return g_variant_new_uint32((guint32)h->uid);
    if (g_str_equal(p,"GID")) return g_variant_new_uint32((guint32)h->gid);
    if (g_str_equal(p,"RealName")) return g_variant_new_string(h->real_name);
    if (g_str_equal(p,"HomeDirectory")) return g_variant_new_string(h->home_directory);
    if (g_str_equal(p,"Shell")) return g_variant_new_string(h->shell);
    if (g_str_equal(p,"State")) return g_variant_new_string("inactive");
    if (g_str_equal(p,"Service")) return g_variant_new_string("nss");
    return NULL;
}
static const GDBusInterfaceVTable manager_vtable={.method_call=method_call,.get_property=get_property};

RclHome *rcl_home_new(void) {
    RclHome *self=g_new0(RclHome,1); GError *e=NULL;
    self->node=g_dbus_node_info_new_for_xml(xml,&e);
    if (!self->node) { g_warning("Invalid home1 introspection data: %s",e->message); g_error_free(e); g_free(self); return NULL; }
    self->homes=g_ptr_array_new_with_free_func(home_entry_free);
    self->homes_by_uid=g_hash_table_new(g_direct_hash,g_direct_equal); return self;
}
void rcl_home_free(RclHome *self) {
    if (!self) return;
    if (self->connection) {
        if (self->registration_id) g_dbus_connection_unregister_object(self->connection,self->registration_id);
        for (guint i=0;i<self->homes->len;i++) { HomeEntry *h=g_ptr_array_index(self->homes,i); if (h->registration_id) g_dbus_connection_unregister_object(self->connection,h->registration_id); }
        g_object_unref(self->connection);
    }
    g_hash_table_unref(self->homes_by_uid); g_ptr_array_unref(self->homes); g_dbus_node_info_unref(self->node); g_free(self);
}
gboolean rcl_home_register(RclHome *self,GDBusConnection *connection,GError **error) {
    struct passwd *pw;
    g_return_val_if_fail(self!=NULL,FALSE); g_return_val_if_fail(G_IS_DBUS_CONNECTION(connection),FALSE);
    if (self->registration_id && self->connection == connection) return TRUE;

    /* A system-bus restart can give us a new connection. Drop registrations
     * tied to the old connection before registering on the replacement. */
    if (self->connection && self->connection != connection) {
        if (self->registration_id) {
            g_dbus_connection_unregister_object(self->connection, self->registration_id);
            self->registration_id = 0;
        }
        for (guint i = 0; i < self->homes->len; i++) {
            HomeEntry *h = g_ptr_array_index(self->homes, i);
            if (h->registration_id) {
                g_dbus_connection_unregister_object(self->connection, h->registration_id);
                h->registration_id = 0;
            }
        }
        g_hash_table_remove_all(self->homes_by_uid);
        g_ptr_array_set_size(self->homes, 0);
        g_clear_object(&self->connection);
    }
    self->connection=g_object_ref(connection);
    self->registration_id=g_dbus_connection_register_object(connection,OBJECT_PATH,self->node->interfaces[0],&manager_vtable,self,NULL,error);
    if (!self->registration_id) { g_clear_object(&self->connection); return FALSE; }
    setpwent();
    while ((pw=getpwent())!=NULL) if (!register_home_entry(self,pw,error)) {
        endpwent(); g_dbus_connection_unregister_object(connection,self->registration_id); self->registration_id=0;
        for (guint i=0;i<self->homes->len;i++) { HomeEntry *h=g_ptr_array_index(self->homes,i); if (h->registration_id) { g_dbus_connection_unregister_object(connection,h->registration_id); h->registration_id=0; } }
        g_hash_table_remove_all(self->homes_by_uid); g_ptr_array_set_size(self->homes,0); g_clear_object(&self->connection); return FALSE;
    }
    endpwent(); return TRUE;
}
