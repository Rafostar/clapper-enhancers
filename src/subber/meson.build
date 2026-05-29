if not clapper_dep.found() or not clapper_dep.version().version_compare('>= 0.11.0')
  if enhancer_option.enabled()
    error('@0@ enhancer was enabled, but Clapper version requirement is not met'.format(name))
  endif
  subdir_done()
endif

config_h = configuration_data()
config_h.set_quoted('CLAPPER_API_NAME', clapper_api_name)
config_h.set_quoted('CLAPPER_SUBBER_RESOURCE_PATH', '/com/github/rafostar/Clapper/Enhancers/clapper-subber')
config_h.set_quoted('CLAPPER_SUBBER_AUTH_HEADER', 'Authorization')
config_h.set_quoted('CLAPPER_SUBBER_LOGIN_HEADER', 'Api-Key')
config_h.set_quoted('CLAPPER_SUBBER_UA_HEADER', 'User-Agent')
config_h.set_quoted('CLAPPER_SUBBER_ACCEPT_HEADER', 'Accept')
config_h.set_quoted('CLAPPER_SUBBER_UA_STRING', 'clapper-subber v' + meson.project_version())

configure_file(output: 'config.h', configuration: config_h)

enhancer_plugin_template = 'clapper-subber.plugin.in'

enhancer_deps += [
  dependency('libsoup-3.0', required: false),
  common_utils_dep,
  json_utils_dep,
]
enhancer_sources += [
  'subber/clapper-subber.c',
  'subber/clapper-subber-ranker.c',
  'subber/clapper-subber.gresources.c',
]
enhancer_configurable = true
