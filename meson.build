project(
  'homed', 'c',
  version: '0.2.0',
  license: 'GPL-2.0-or-later',
  meson_version: '>= 0.58.0',
  default_options: ['c_std=gnu11', 'warning_level=2', 'werror=false'],
)

cc = meson.get_compiler('c')
add_project_arguments(cc.get_supported_arguments([
  '-Wno-unused-parameter', '-Wno-missing-field-initializers',
  '-fstack-protector-strong', '-D_FORTIFY_SOURCE=2',
]), language: 'c')

glib_req = '>= 2.56'
glib_dep = dependency('glib-2.0', version: glib_req)
gio_dep = dependency('gio-2.0', version: glib_req)
gio_unix = dependency('gio-unix-2.0', version: glib_req)
polkit = dependency('polkit-gobject-1', version: '>= 0.113')

prefix = get_option('prefix')
datadir = get_option('datadir')
libexecdir = get_option('libexecdir')
conf = configuration_data()
conf.set_quoted('PACKAGE', meson.project_name())
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('PRIVILEGED_GROUP', get_option('privileged-group'))
configure_file(output: 'config.h', configuration: conf)
config_h_inc = include_directories('.')
subdir('src')
subdir('data')
if get_option('tests')
  subdir('tests')
endif
install_data('README.md', 'AUTHORS', 'COPYING', 'ChangeLog.md',
  install_dir: join_paths(get_option('datadir'), 'doc', 'homed'))
summary({'Tests': get_option('tests'), 'Privileged group': get_option('privileged-group')}, section: 'Configuration')
