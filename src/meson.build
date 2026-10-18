homed_sources = files('main.c', 'rcl-home.c')
executable('homed', homed_sources,
  dependencies: [glib_dep, gio_dep, gio_unix, polkit],
  include_directories: [config_h_inc, include_directories('.')],
  install: true, install_dir: get_option('libexecdir'),
  gnu_symbol_visibility: 'hidden',
  c_args: ['-DGLIB_DISABLE_DEPRECATION_WARNINGS'])
