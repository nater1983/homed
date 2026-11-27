#include "rcl-nss.h"

#include <dlfcn.h>
#include <stdbool.h>

/*
 * elogind's NSS module provides a thread-local, nestable recursion guard.
 * Resolve it at runtime: homed must also work on Slackware installations
 * without libnss_elogind.  Keep the module loaded for the daemon lifetime.
 *
 * The guard must be enabled on the thread that performs the NSS lookup;
 * the process-wide mutex alone cannot prevent a lookup from calling back
 * through elogind-userdbd into this Varlink provider.
 */
typedef int (*NssElogindBlockFunc)(bool);
static GMutex nss_mutex;
static NssElogindBlockFunc elogind_block;
static gsize elogind_block_initialized;
static GPrivate elogind_block_active = G_PRIVATE_INIT(NULL);

static void
initialize_elogind_block(void)
{
    if (g_once_init_enter(&elogind_block_initialized)) {
        void *module = dlopen("libnss_elogind.so.2", RTLD_NOW | RTLD_LOCAL);
        if (module != NULL) {
            void *symbol = dlsym(module, "_nss_elogind_block");
            if (symbol != NULL)
                *(void **) (&elogind_block) = symbol;
            else
                dlclose(module);
        }
        g_once_init_leave(&elogind_block_initialized, 1);
    }
}

static void
block_elogind_lookups(void)
{
    initialize_elogind_block();
    if (elogind_block != NULL && elogind_block(true) >= 0)
        g_private_set(&elogind_block_active, GINT_TO_POINTER(1));
}

static void
unblock_elogind_lookups(void)
{
    if (g_private_get(&elogind_block_active) != NULL) {
        g_private_set(&elogind_block_active, NULL);
        (void) elogind_block(false);
    }
}

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
    block_elogind_lookups();
}

gboolean
rcl_nss_lock_timeout(guint timeout_ms)
{
    gint64 deadline = g_get_monotonic_time() + ((gint64) timeout_ms * 1000);

    do {
        if (g_mutex_trylock(&nss_mutex)) {
            block_elogind_lookups();
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

    if (!g_mutex_trylock(&nss_mutex))
        return FALSE;
    block_elogind_lookups();
    return TRUE;
}

void
rcl_nss_unlock(void)
{
    unblock_elogind_lookups();
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
    /* Preserve the account class used by userdb consumers such as GDM.
     * Slackware reserves UIDs below 1000 for system users; root and nobody
     * have intrinsic dispositions in the user record specification. */
    const gchar *disposition =
        (user->pw_uid == 0 || user->pw_uid == 65534) ? "intrinsic" :
        user->pw_uid < 1000 ? "system" : "regular";

    json_object_set_string_member(record, "disposition", disposition);

    return record;
}
