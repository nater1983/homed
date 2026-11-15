#include "rcl-nss.h"

static GMutex nss_mutex;

#ifdef HOMED_TESTING
static RclNssTestCallback test_callback;

void
rcl_nss_test_set_callback(RclNssTestCallback callback)
{
    test_callback = callback;
}
#endif

void
rcl_nss_lock(void)
{
    g_mutex_lock(&nss_mutex);
}

gboolean
rcl_nss_lock_timeout(guint timeout_ms)
{
    gint64 deadline = g_get_monotonic_time() + ((gint64) timeout_ms * 1000);

    do {
        if (g_mutex_trylock(&nss_mutex)) {
#ifdef HOMED_TESTING
            if (test_callback != NULL) {
                RclNssTestCallback callback = test_callback;
                test_callback = NULL;
                callback();
            }
#endif
            return TRUE;
        }
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);

    return g_mutex_trylock(&nss_mutex);
}

void
rcl_nss_unlock(void)
{
    g_mutex_unlock(&nss_mutex);
}

const gchar *
rcl_nss_user_shell(const struct passwd *user)
{
    return user->pw_shell != NULL && user->pw_shell[0] != '\0'
        ? user->pw_shell : "/bin/sh";
}

JsonObject *
rcl_nss_user_record(const struct passwd *user, const gchar *service)
{
    JsonObject *record = json_object_new();

    json_object_set_string_member(record, "userName", user->pw_name ? user->pw_name : "");
    json_object_set_int_member(record, "uid", user->pw_uid);
    json_object_set_int_member(record, "gid", user->pw_gid);
    json_object_set_string_member(record, "realName", user->pw_gecos ? user->pw_gecos : "");
    json_object_set_string_member(record, "homeDirectory", user->pw_dir ? user->pw_dir : "");
    json_object_set_string_member(record, "shell", rcl_nss_user_shell(user));
    json_object_set_string_member(record, "service", service);
    json_object_set_string_member(record, "disposition", "regular");

    return record;
}
