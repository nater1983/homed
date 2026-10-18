service_opt = get_option('dbus_system_service_dir')
if service_opt != ''
  dbus_service_dir = service_opt
else
  dbus_dep = dependency('dbus-1', required: false)
  if dbus_dep.found()
    dbus_service_dir = dbus_dep.get_variable(pkgconfig: 'system_bus_services_dir', pkgconfig_define: ['datadir', join_paths(prefix, datadir)])
  else
    dbus_service_dir = join_paths(prefix, datadir, 'dbus-1', 'system-services')
  endif
endif
conf_opt = get_option('dbus_system_conf_dir')
dbus_conf_dir = conf_opt != '' ? conf_opt : join_paths(prefix, datadir, 'dbus-1', 'system.d')
polkit_opt = get_option('polkit_policy_dir')
if polkit_opt != ''
  polkit_dir = polkit_opt
else
  polkit_dir = polkit.get_variable(pkgconfig: 'actiondir', pkgconfig_define: ['datadir', join_paths(prefix, datadir)])
endif
service_conf = configuration_data()
service_conf.set('LIBEXECDIR', join_paths(prefix, get_option('libexecdir')))
configure_file(input: 'org.freedesktop.home1.service.in', output: 'org.freedesktop.home1.service', configuration: service_conf, install: true, install_dir: dbus_service_dir)
install_data('org.freedesktop.home1.conf', install_dir: dbus_conf_dir)
install_data('org.freedesktop.home1.xml', install_dir: join_paths(prefix, datadir, 'dbus-1', 'interfaces'))
install_data('org.freedesktop.home1.policy', install_dir: polkit_dir)
rule_conf = configuration_data()
rule_conf.set('PRIVILEGED_GROUP', get_option('privileged-group'))
rules = configure_file(input: 'org.freedesktop.home1.rules.in', output: 'org.freedesktop.home1.rules', configuration: rule_conf)
install_data(rules, install_dir: join_paths(prefix, datadir, 'polkit-1', 'rules.d'))
