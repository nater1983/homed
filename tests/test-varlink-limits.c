#include "rcl-varlink-limits.h"

static void
test_per_uid_accounting(void)
{
    const uid_t root_uid = 0;
    const uid_t other_uid = 1000;
    RclVarlinkLimits *limits = rcl_varlink_limits_new();

    g_assert_true(rcl_varlink_limits_acquire_uid_for_test(limits, root_uid));
    g_assert_true(rcl_varlink_limits_acquire_uid_for_test(limits, root_uid));
    g_assert_false(rcl_varlink_limits_acquire_uid_for_test(limits, root_uid));
    g_assert_true(rcl_varlink_limits_acquire_uid_for_test(limits, other_uid));
    g_assert_true(rcl_varlink_limits_acquire_uid_for_test(limits, other_uid));
    g_assert_false(rcl_varlink_limits_acquire_uid_for_test(limits, other_uid));

    rcl_varlink_limits_release(limits, root_uid);
    g_assert_false(rcl_varlink_limits_acquire_uid_for_test(limits, other_uid));
    g_assert_true(rcl_varlink_limits_acquire_uid_for_test(limits, root_uid));
    g_assert_false(rcl_varlink_limits_acquire_uid_for_test(limits, root_uid));

    rcl_varlink_limits_release(limits, other_uid);
    rcl_varlink_limits_release(limits, other_uid);
    g_assert_false(rcl_varlink_limits_acquire_uid_for_test(limits, root_uid));
    g_assert_true(rcl_varlink_limits_acquire_uid_for_test(limits, other_uid));

    rcl_varlink_limits_release(limits, other_uid);
    rcl_varlink_limits_release(limits, root_uid);
    rcl_varlink_limits_release(limits, root_uid);
    rcl_varlink_limits_free(limits);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/varlink/limits/per-uid-accounting", test_per_uid_accounting);
    return g_test_run();
}
