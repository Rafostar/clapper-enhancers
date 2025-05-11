enhancer_plugin_template = 'clapper-peertube.plugin.in'

enhancer_deps += [
  dependency('gstreamer-1.0', version: '>= 1.20.0', required: false),
  dependency('gstreamer-tag-1.0', version: '>= 1.20.0', required: false),
  dependency('libsoup-3.0', required: false),
  common_utils_dep,
  json_utils_dep,
]
enhancer_sources += [
  'peertube/clapper-peertube.c',
]
