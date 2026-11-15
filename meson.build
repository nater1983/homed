project(
  'homed', 'c',
  version: '0.4.beta',
  license: 'GPL-2.0-or-later',
  meson_version: '>= 0.58.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'werror=false',
    'optimization=2',
  ],
)

cc = meson.get_compiler('c')
extra_c_args = cc.get_supported_arguments([
  '-Wno-unused-parameter', '-Wno-missing-field-initializers',
  '-fstack-protector-strong',
])

# Enable fortified libc functions only when optimization is active.
optimized_src = '''
  #if !defined(__OPTIMIZE__) || __OPTIMIZE__ == 0
  #error Optimization is disabled
  #endif
'''

# Meson disables optimization in compiler checks. Match the target flags,
# including optimization overrides supplied through CFLAGS or c_args.
optimization = get_option('optimization')
optimization_args = []
if optimization != 'plain'
  optimization_args += ['-O' + optimization]
endif
foreach arg : get_option('c_args')
  if arg.startswith('-O')
    optimization_args += [arg]
  endif
endforeach

if cc.compiles(optimized_src, args: optimization_args, name: 'compiler optimization enabled')
  extra_c_args += cc.get_supported_arguments(['-D_FORTIFY_SOURCE=2'])
endif

if get_option('buildtype').contains('debug')
  extra_c_args += cc.get_supported_arguments([
    '-Wcast-align',
    '-Wmissing-declarations',
    '-Wmissing-prototypes',
    '-Wnested-externs',
  ])
endif

add_project_arguments(extra_c_args, language: 'c')

glib_req = '>= 2.56'
glib_dep = dependency('glib-2.0', version: glib_req)
gio_dep = dependency('gio-2.0', version: glib_req)
gio_unix = dependency('gio-unix-2.0', version: glib_req)
json_glib = dependency('json-glib-1.0', version: '>= 1.4')
polkit = dependency('polkit-gobject-1', version: '>= 0.113')

prefix = get_option('prefix')
datadir = get_option('datadir')
libexecdir = get_option('libexecdir')
conf = configuration_data()
conf.set_quoted('PACKAGE', meson.project_name())
conf.set_quoted('PACKAGE_VERSION', meson.project_version())
conf.set_quoted('PRIVILEGED_GROUP', get_option('privileged-group'))
conf.set_quoted('VARLINK_VENDOR', get_option('varlink-vendor'))
configure_file(output: 'config.h', configuration: conf)
config_h_inc = include_directories('.')
subdir('src')
install_man('man/homed.8')
subdir('data')
if get_option('tests')
  subdir('tests')
endif
summary({'Tests': get_option('tests'), 'Privileged group': get_option('privileged-group'), 'Varlink vendor': get_option('varlink-vendor')}, section: 'Configuration')
