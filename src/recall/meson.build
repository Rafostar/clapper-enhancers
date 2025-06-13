if not clapper_dep.found() or not clapper_dep.version().version_compare('>= 0.9.0')
  if enhancer_option.enabled()
    error('@0@ enhancer was enabled, but Clapper version requirement is not met'.format(name))
  endif
  subdir_done()
endif

config_h = configuration_data()
config_h.set_quoted('CLAPPER_API_NAME', clapper_api_name)

configure_file(output: 'config.h', configuration: config_h)

enhancer_plugin_template = 'clapper-recall.plugin.in'

enhancer_deps += [
  dependency('sqlite3', required: false),
]
enhancer_sources += [
  'recall/clapper-recall.c',
]
enhancer_configurable = true
