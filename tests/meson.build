python = find_program('python3')

test('dbus-xml-valid', python,
  args: [files('check-xml.py'),
         join_paths(meson.project_source_root(), 'data', 'org.freedesktop.home1.xml'),
         join_paths(meson.project_source_root(), 'data', 'org.freedesktop.home1.conf'),
         join_paths(meson.project_source_root(), 'data', 'org.freedesktop.home1.policy')])

test_varlink = executable('test-varlink',
  'test-varlink.c',
  '../src/rcl-varlink.c',
  '../src/rcl-nss.c',
  dependencies: [glib_dep, gio_dep, gio_unix, json_glib],
  include_directories: [include_directories('../src'), config_h_inc],
  c_args: ['-DHOMED_TESTING'])

test('varlink-integration', test_varlink, timeout: 60)

test_dbus = executable('test-dbus',
  'test-dbus.c',
  '../src/rcl-home.c',
  '../src/rcl-varlink.c',
  '../src/rcl-nss.c',
  dependencies: [glib_dep, gio_dep, gio_unix, json_glib],
  include_directories: [include_directories('../src'), config_h_inc],
  c_args: ['-DHOMED_TESTING'])

test('dbus-integration', test_dbus, timeout: 60)
