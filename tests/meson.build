python = find_program('python3')
test('dbus-xml-valid', python,
  args: [files('check-xml.py'),
         join_paths(meson.project_source_root(), 'data', 'org.freedesktop.home1.xml'),
         join_paths(meson.project_source_root(), 'data', 'org.freedesktop.home1.conf'),
         join_paths(meson.project_source_root(), 'data', 'org.freedesktop.home1.policy')])
