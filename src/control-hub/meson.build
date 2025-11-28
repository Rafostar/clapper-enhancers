if not clapper_dep.found() or not clapper_dep.version().version_compare('>= 0.9.0')
  if enhancer_option.enabled()
    error('@0@ enhancer was enabled, but Clapper version requirement is not met'.format(name))
  endif
  subdir_done()
endif

config_h = configuration_data()
config_h.set_quoted('CONTROL_HUB_VERSION_S', meson.project_version())

configure_file(output: 'config.h', configuration: config_h)

enhancer_plugin_template = 'clapper-control-hub.plugin.in'

enhancer_deps += [
  dependency('gstreamer-1.0', version: '>= 1.20.0', required: false),
  dependency('gstreamer-tag-1.0', version: '>= 1.20.0', required: false),
  dependency('libsoup-3.0', required: false),
  dependency('microdns', version: '>= 0.2.0', required: false),
]
enhancer_sources += [
  'control-hub/clapper-control-hub.c',
  'control-hub/clapper-control-hub-actions.c',
  'control-hub/clapper-control-hub-json.c',
  'control-hub/clapper-control-hub-mdns.c',
  'control-hub/clapper-control-hub-ws.c',
]
enhancer_configurable = true
