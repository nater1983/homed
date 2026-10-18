#include "rcl-home.h"
#include <pwd.h>
#include <unistd.h>
#include <polkit/polkit.h>

#define OBJECT_PATH "/org/freedesktop/home1"
#define IFACE "org.freedesktop.home1.Manager"

struct _RclHome { GDBusNodeInfo *node; guint registration_id; };

static const gchar xml[] =
"<node><interface name='org.freedesktop.home1.Manager'>"
" <method name='GetUserRecordByUID'><arg name='uid' type='u' direction='in'/><arg name='record' type='a{sv}' direction='out'/><arg name='incomplete' type='b' direction='out'/></method>"
"<method name='GetUserRecordByName'><arg name='user_name' type='s' direction='in'/><arg name='record' type='a{sv}' direction='out'/><arg name='incomplete' type='b' direction='out'/></method>"
"<method name='GetHomeByName'><arg name='user_name' type='s' direction='in'/><arg name='uid' type='u' direction='out'/><arg name='home_state' type='s' direction='out'/><arg name='gid' type='u' direction='out'/><arg name='real_name' type='s' direction='out'/><arg name='home_directory' type='s' direction='out'/><arg name='shell' type='s' direction='out'/><arg name='bus_path' type='o' direction='out'/></method>"
"<method name='GetHomeByUID'><arg name='uid' type='u' direction='in'/><arg name='user_name' type='s' direction='out'/><arg name='home_state' type='s' direction='out'/><arg name='gid' type='u' direction='out'/><arg name='real_name' type='s' direction='out'/><arg name='home_directory' type='s' direction='out'/><arg name='shell' type='s' direction='out'/><arg name='bus_path' type='o' direction='out'/></method>"
"<method name='ListHomes'><arg name='home_areas' type='a(susussso)' direction='out'/></method>"
"<property name='AutoLogin' type='a(sso)' access='read'/>"
"</interface></node>";

static gchar *
home_object_path(uid_t uid)
{
    return g_strdup_printf("/org/freedesktop/home1/home/_%u", (guint) uid);
}

static void
return_user(GDBusMethodInvocation *inv, struct passwd *pw, gboolean name_first)
{
    gchar *path = home_object_path(pw->pw_uid);
    const gchar *gecos = pw->pw_gecos ? pw->pw_gecos : "";
    const gchar *shell = (pw->pw_shell && *pw->pw_shell) ? pw->pw_shell : "/bin/sh";
    if (name_first)
        g_dbus_method_invocation_return_value(inv, g_variant_new("(ssusssso)",
            pw->pw_name, "inactive", (guint) pw->pw_gid, gecos,
            pw->pw_dir, shell, path));
    else
        g_dbus_method_invocation_return_value(inv, g_variant_new("(ususssso)",
            pw->pw_name, "inactive", (guint) pw->pw_gid, gecos,
            pw->pw_dir, shell, path));
    g_free(path);
}

static void
return_user_record(GDBusMethodInvocation *inv, struct passwd *pw)
{
    GVariantBuilder record;
    g_variant_builder_init(&record, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&record, "{sv}", "userName", g_variant_new_string(pw->pw_name ? pw->pw_name : ""));
    g_variant_builder_add(&record, "{sv}", "uid", g_variant_new_uint32((guint32) pw->pw_uid));
    g_variant_builder_add(&record, "{sv}", "gid", g_variant_new_uint32((guint32) pw->pw_gid));
    g_variant_builder_add(&record, "{sv}", "realName", g_variant_new_string(pw->pw_gecos ? pw->pw_gecos : ""));
    g_variant_builder_add(&record, "{sv}", "homeDirectory", g_variant_new_string(pw->pw_dir ? pw->pw_dir : ""));
    g_variant_builder_add(&record, "{sv}", "shell", g_variant_new_string((pw->pw_shell && *pw->pw_shell) ? pw->pw_shell : "/bin/sh"));
    g_variant_builder_add(&record, "{sv}", "service", g_variant_new_string("nss"));
    g_dbus_method_invocation_return_value(inv, g_variant_new("(a{sv}b)", &record, FALSE));
}

static void
method_call(GDBusConnection *conn, const gchar *sender, const gchar *obj,
            const gchar *iface, const gchar *method, GVariant *params,
            GDBusMethodInvocation *inv, gpointer user_data)
{
    (void) conn; (void) sender; (void) obj; (void) iface; (void) user_data;
    if (g_str_equal(method, "GetUserRecordByUID")) {
        guint uid;
        g_variant_get(params, "(u)", &uid);
        struct passwd *pw = getpwuid((uid_t) uid);
        if (!pw) {
            g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.home1.NoSuchUser", "No local account with that UID");
            return;
        }
        return_user_record(inv, pw);
        return;
    }
    if (g_str_equal(method, "GetUserRecordByName")) {
        const gchar *name;
        g_variant_get(params, "(&s)", &name);
        struct passwd *pw = getpwnam(name);
        if (!pw) {
            g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.home1.NoSuchUser", "No local account with that name");
            return;
        }
        return_user_record(inv, pw);
        return;
    }
    if (g_str_equal(method, "GetHomeByName")) {
        const gchar *name;
        g_variant_get(params, "(&s)", &name);
        struct passwd *pw = getpwnam(name);
        if (!pw) {
            g_dbus_method_invocation_return_dbus_error(inv,
                "org.freedesktop.home1.NoSuchHome", "No local account with that name");
            return;
        }
        return_user(inv, pw, FALSE);
        return;
    }
    if (g_str_equal(method, "GetHomeByUID")) {
        guint uid;
        g_variant_get(params, "(u)", &uid);
        struct passwd *pw = getpwuid((uid_t) uid);
        if (!pw) {
            g_dbus_method_invocation_return_dbus_error(inv,
                "org.freedesktop.home1.NoSuchHome", "No local account with that UID");
            return;
        }
        return_user(inv, pw, TRUE);
        return;
    }
    if (g_str_equal(method, "ListHomes")) {
        GVariantBuilder b;
        struct passwd *pw;
        g_variant_builder_init(&b, G_VARIANT_TYPE("a(susussso)"));
        setpwent();
        while ((pw = getpwent()) != NULL) {
            gchar *path = home_object_path(pw->pw_uid);
            g_variant_builder_add(&b, "(susussso)", pw->pw_name,
                (guint) pw->pw_uid, "inactive", (guint) pw->pw_gid,
                pw->pw_gecos ? pw->pw_gecos : "", pw->pw_dir,
                (pw->pw_shell && *pw->pw_shell) ? pw->pw_shell : "/bin/sh", path);
            g_free(path);
        }
        endpwent();
        g_dbus_method_invocation_return_value(inv, g_variant_new("(a(susussso))", &b));
        return;
    }
    g_dbus_method_invocation_return_dbus_error(inv,
        "org.freedesktop.DBus.Error.NotSupported",
        "This initial compatibility service does not implement home-area management");
}

static GVariant *
get_property(GDBusConnection *conn, const gchar *sender, const gchar *obj,
             const gchar *iface, const gchar *property, GError **error, gpointer data)
{
    (void) conn; (void) sender; (void) obj; (void) iface; (void) error; (void) data;
    if (g_str_equal(property, "AutoLogin"))
        return g_variant_new_array(G_VARIANT_TYPE("(sso)"), NULL, 0);
    return NULL;
}

static const GDBusInterfaceVTable vtable = {
    .method_call = method_call,
    .get_property = get_property,
};

RclHome *
rcl_home_new(void)
{
    RclHome *self = g_new0(RclHome, 1);
    GError *error = NULL;
    self->node = g_dbus_node_info_new_for_xml(xml, &error);
    if (!self->node) {
        g_warning("Invalid home1 introspection data: %s", error->message);
        g_error_free(error);
        g_free(self);
        return NULL;
    }
    return self;
}

void
rcl_home_free(RclHome *self)
{
    if (!self) return;
    if (self->node) g_dbus_node_info_unref(self->node);
    g_free(self);
}

gboolean
rcl_home_register(RclHome *self, GDBusConnection *connection, GError **error)
{
    g_return_val_if_fail(self != NULL, FALSE);
    self->registration_id = g_dbus_connection_register_object(connection,
        OBJECT_PATH, self->node->interfaces[0], &vtable, self, NULL, error);
    return self->registration_id != 0;
}
