#include <glib.h>
#include <dlfcn.h>
#include <stdbool.h>
#include <pwd.h>
#include <errno.h>
#include <string.h>

#include "rcl-nss.h"

/* These are optional, elogind-specific symbols, not public glibc APIs. */
typedef bool (*IsBlockedFunc)(void);
typedef int (*GetpwnamFunc)(const char *, struct passwd *, char *, size_t, int *);

static void *elogind_module;
static IsBlockedFunc is_blocked;
static GetpwnamFunc elogind_getpwnam;

static gboolean
require_elogind(void)
{
    if (is_blocked != NULL)
        return TRUE;
    g_test_skip("libnss_elogind or its recursion-guard API is unavailable");
    return FALSE;
}

static void
test_nss_lock(void)
{
    if (!require_elogind())
        return;

    g_assert_false(is_blocked());
    rcl_nss_lock();
    g_assert_true(is_blocked());

    struct passwd *pw = getpwnam("root");
    g_assert_nonnull(pw);
    g_assert_cmpuint(pw->pw_uid, ==, 0);
    g_assert_true(is_blocked());

    rcl_nss_unlock();
    g_assert_false(is_blocked());
}

static void
test_nss_timeout_lock(void)
{
    if (!require_elogind())
        return;

    g_assert_false(is_blocked());
    g_assert_true(rcl_nss_lock_timeout(1000));
    g_assert_true(is_blocked());
    rcl_nss_unlock();
    g_assert_false(is_blocked());
}

static void
test_repeated_locking(void)
{
    if (!require_elogind())
        return;

    for (int i = 0; i < 100; i++) {
        rcl_nss_lock();
        g_assert_true(is_blocked());
        rcl_nss_unlock();
        g_assert_false(is_blocked());
    }
}

static void
test_elogind_provider_blocked(void)
{
    if (!require_elogind())
        return;
    if (elogind_getpwnam == NULL) {
        g_test_skip("elogind NSS getpwnam provider is unavailable");
        return;
    }

    struct passwd pw = {0};
    char buffer[16384];
    int error_number = 0;

    /* Exercise the provider itself, not glibc's system NSS configuration.
     * NSS_STATUS_UNAVAIL is -1 (the glibc nss_status enum).
     */
    g_assert_false(is_blocked());
    rcl_nss_lock();
    g_assert_true(is_blocked());
    int status = elogind_getpwnam("root", &pw, buffer, sizeof(buffer),
                                  &error_number);
    g_assert_cmpint(status, ==, -1);
    g_assert_true(is_blocked());
    rcl_nss_unlock();
    g_assert_false(is_blocked());
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    elogind_module = dlopen("libnss_elogind.so.2", RTLD_NOW | RTLD_LOCAL);
    if (elogind_module != NULL) {
        void *symbol = dlsym(elogind_module, "_nss_elogind_is_blocked");
        if (symbol != NULL)
            memcpy(&is_blocked, &symbol, sizeof(is_blocked));
        symbol = dlsym(elogind_module, "_nss_elogind_getpwnam_r");
        if (symbol != NULL)
            memcpy(&elogind_getpwnam, &symbol, sizeof(elogind_getpwnam));
    }

    g_test_add_func("/phase6/nss-lock", test_nss_lock);
    g_test_add_func("/phase6/nss-timeout-lock", test_nss_timeout_lock);
    g_test_add_func("/phase6/repeated-locking", test_repeated_locking);
    g_test_add_func("/phase6/elogind-provider-blocked",
                    test_elogind_provider_blocked);

    int result = g_test_run();
    if (elogind_module != NULL)
        dlclose(elogind_module);
    return result;
}
