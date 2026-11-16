#include "config.h"

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>

#define HOME1_NAME "org.freedesktop.home1"
#define HOME1_PATH "/org/freedesktop/home1"
#define HOME1_MANAGER HOME1_NAME ".Manager"

static gboolean
print_json(JsonNode *node, GError **error)
{
    g_autofree gchar *json = json_to_string(node, FALSE);

    /* Keep JSON in UTF-8 even when the caller uses a different locale. */
    if (puts(json) == EOF || fflush(stdout) == EOF) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Could not write JSON to standard output");
        return FALSE;
    }
    return TRUE;
}

static gboolean
list_homes(GVariant *reply, gboolean json, GError **error)
{
    g_autoptr(GVariantIter) homes = NULL;
    g_autoptr(JsonNode) output = NULL;
    JsonArray *records = NULL;
    const gchar *name, *state, *real_name, *directory, *shell, *path;
    guint32 uid, gid;

    if (json) {
        output = json_node_new(JSON_NODE_ARRAY);
        records = json_array_new();
        json_node_take_array(output, records);
    } else {
        g_print("NAME\tUID\tGID\tSTATE\tHOME\tSHELL\n");
    }

    g_variant_get(reply, "(a(susussso))", &homes);
    while (g_variant_iter_loop(homes, "(&su&su&s&s&s&o)", &name, &uid, &state,
                               &gid, &real_name, &directory, &shell, &path)) {
        if (json) {
            JsonObject *record = json_object_new();
            json_object_set_string_member(record, "userName", name);
            json_object_set_int_member(record, "uid", uid);
            json_object_set_int_member(record, "gid", gid);
            json_object_set_string_member(record, "state", state);
            json_object_set_string_member(record, "realName", real_name);
            json_object_set_string_member(record, "homeDirectory", directory);
            json_object_set_string_member(record, "shell", shell);
            json_object_set_string_member(record, "objectPath", path);
            json_array_add_object_element(records, record);
        } else {
            g_print("%s\t%u\t%u\t%s\t%s\t%s\n",
                    name, uid, gid, state, directory, shell);
        }
    }

    return !json || print_json(output, error);
}

static gboolean
inspect_home(GVariant *reply, gboolean json, GError **error)
{
    const gchar *record_json, *path;
    gboolean incomplete;

    g_variant_get(reply, "(&sb&o)", &record_json, &incomplete, &path);
    g_autoptr(JsonNode) node = json_from_string(record_json, error);
    if (node == NULL)
        return FALSE;
    if (!JSON_NODE_HOLDS_OBJECT(node)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "The service returned a user record that is not a JSON object");
        return FALSE;
    }
    if (json)
        return print_json(node, error);

    static const struct {
        const gchar *key;
        const gchar *label;
    } fields[] = {
        { "userName", "User name" },
        { "uid", "UID" },
        { "gid", "GID" },
        { "realName", "Real name" },
        { "homeDirectory", "Home directory" },
        { "shell", "Shell" },
        { "service", "Service" },
    };
    JsonObject *record = json_node_get_object(node);
    for (guint i = 0; i < G_N_ELEMENTS(fields); i++) {
        JsonNode *value = json_object_get_member(record, fields[i].key);
        if (value == NULL || !JSON_NODE_HOLDS_VALUE(value))
            continue;
        if (json_node_get_value_type(value) == G_TYPE_STRING)
            g_print("%14s: %s\n", fields[i].label, json_node_get_string(value));
        else if (json_node_get_value_type(value) == G_TYPE_INT64)
            g_print("%14s: %" G_GINT64_FORMAT "\n", fields[i].label, json_node_get_int(value));
    }
    g_print("%14s: %s\n", "Object path", path);
    g_print("%14s: %s\n", "Incomplete", incomplete ? "yes" : "no");
    return TRUE;
}

int
main(int argc, char **argv)
{
    gboolean json = FALSE;
    gboolean version = FALSE;
    GOptionEntry options[] = {
        { "json", 'j', 0, G_OPTION_ARG_NONE, &json, "Print JSON", NULL },
        { "version", 0, 0, G_OPTION_ARG_NONE, &version, "Print version", NULL },
        { NULL }
    };
    setlocale(LC_ALL, "");
    g_autoptr(GOptionContext) context = g_option_context_new("[list | inspect USER]");
    g_option_context_set_summary(context, "List and inspect home accounts over the system bus.");
    g_option_context_add_main_entries(context, options, NULL);

    g_autoptr(GError) error = NULL;
    g_autoptr(GVariant) parameters = NULL;
    g_autoptr(GDBusConnection) connection = NULL;
    g_autoptr(GVariant) reply = NULL;
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("homectl: %s (see --help)\n", error->message);
        return 2;
    }
    if (version) {
        g_print("homectl %s\n", PACKAGE_VERSION);
        return 0;
    }

    gboolean list = argc == 1 || (argc == 2 && g_str_equal(argv[1], "list"));
    const gchar *method = "ListHomes";
    const GVariantType *type = G_VARIANT_TYPE("(a(susussso))");
    if (!list) {
        if (argc != 3 || !g_str_equal(argv[1], "inspect")) {
            g_printerr("homectl: use 'list' or 'inspect USER' (see --help)\n");
            return 2;
        }
        const gchar *user = argv[2];
        if (*user != '\0' && strspn(user, "0123456789") == strlen(user)) {
            guint64 uid = g_ascii_strtoull(user, NULL, 10);
            if (uid > G_MAXUINT32) {
                g_printerr("homectl: UID out of range: %s\n", user);
                return 2;
            }
            method = "GetUserRecordByUID";
            parameters = g_variant_ref_sink(g_variant_new("(u)", (guint32) uid));
        } else {
            g_autofree gchar *name = g_locale_to_utf8(user, -1, NULL, NULL, &error);
            if (name == NULL)
                goto fail;
            method = "GetUserRecordByName";
            parameters = g_variant_ref_sink(g_variant_new("(s)", name));
        }
        type = G_VARIANT_TYPE("(sbo)");
    }

    connection = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
    if (connection == NULL)
        goto fail;

    reply = g_dbus_connection_call_sync(connection, HOME1_NAME, HOME1_PATH, HOME1_MANAGER,
        method, parameters, type, G_DBUS_CALL_FLAGS_NONE, -1, NULL, &error);
    if (reply != NULL && (list ? list_homes(reply, json, &error) : inspect_home(reply, json, &error)))
        return 0;

fail:
    g_dbus_error_strip_remote_error(error);
    g_printerr("homectl: %s\n", error->message);
    return 1;
}
