python = find_program('python3')

test('dbus-xml-valid', python,
  args: [files('check-xml.py'),
         join_paths(meson.project_source_root(), 'data', 'org.freedesktop.home1.xml'),
         join_paths(meson.project_source_root(), 'data', 'org.freedesktop.home1.conf'),
         join_paths(meson.project_source_root(), 'data', 'org.freedesktop.home1.policy')])

test_varlink = executable('test-varlink',
  'test-varlink.c',
  '../src/rcl-varlink.c',
  '../src/rcl-varlink-limits.c',
  '../src/rcl-varlink-worker.c',
  '../src/rcl-varlink-server.c',
  '../src/rcl-nss.c',
  dependencies: [glib_dep, gio_dep, gio_unix, json_glib],
  include_directories: [include_directories('../src'), config_h_inc],
  c_args: ['-DHOMED_TESTING'])

test('varlink-integration', test_varlink, timeout: 60)

test_varlink_limits = executable('test-varlink-limits',
  'test-varlink-limits.c',
  '../src/rcl-varlink-limits.c',
  dependencies: [glib_dep, gio_dep],
  include_directories: include_directories('../src'),
  c_args: ['-DHOMED_TESTING', '-DRCL_VARLINK_PER_UID_LIMIT=2'])

test('varlink-limits', test_varlink_limits)

test_dbus = executable('test-dbus',
  'test-dbus.c',
  '../src/rcl-home.c',
  '../src/rcl-varlink.c',
  '../src/rcl-varlink-limits.c',
  '../src/rcl-varlink-worker.c',
  '../src/rcl-varlink-server.c',
  '../src/rcl-nss.c',
  dependencies: [glib_dep, gio_dep, gio_unix, json_glib],
  include_directories: [include_directories('../src'), config_h_inc],
  c_args: ['-DHOMED_TESTING'])

dbus_test_env = environment()
dbus_test_dependencies = []
if get_option('homectl')
  dbus_test_env.set('HOMED_TEST_HOMECTL', homectl.full_path())
  dbus_test_dependencies += [homectl]
endif
test('dbus-integration', test_dbus, timeout: 60,
  env: dbus_test_env, depends: dbus_test_dependencies)

# Phase 6: exercise the production NSS guard against the real elogind module.
# The test skips elogind-specific assertions if the optional NSS module is absent.
test_nss_elogind = executable('test-nss-elogind',
  'test-nss-elogind.c',
  '../src/rcl-nss.c',
  dependencies: [glib_dep, json_glib],
  include_directories: include_directories('../src'))

test('nss-elogind-integration', test_nss_elogind, timeout: 30)
