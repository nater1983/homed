python = find_program('python3')
test('dbus-xml-valid', python,
  args: [files('check-xml.py'),
         join_paths(meson.project_source_root(), 'data', 'org.freedesktop.home1.xml'),
         join_paths(meson.project_source_root(), 'data', 'org.freedesktop.home1.conf'),
         join_paths(meson.project_source_root(), 'data', 'org.freedesktop.home1.policy')])

varlink_test_deps = [glib_dep, gio_dep, json_glib]
varlink_test_inc = include_directories('../src')

varlink_limits_test = executable('test-varlink-limits',
  'test-varlink-limits.c', '../src/rcl-varlink-limits.c',
  dependencies: varlink_test_deps,
  include_directories: varlink_test_inc,
  c_args: ['-DHOMED_TESTING', '-DRCL_VARLINK_PER_UID_LIMIT=2'])
test('varlink-limits', varlink_limits_test)

varlink_test = executable('test-varlink',
  'test-varlink.c', '../src/rcl-varlink.c', '../src/rcl-nss.c',
  '../src/rcl-varlink-worker.c', '../src/rcl-varlink-limits.c',
  dependencies: varlink_test_deps,
  include_directories: varlink_test_inc,
  c_args: ['-DHOMED_TESTING'])
test('varlink-integration', varlink_test)
