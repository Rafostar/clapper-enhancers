if not clapper_dep.found() or not clapper_dep.version().version_compare('>= 0.9.0')
  if enhancer_option.enabled()
    error('@0@ enhancer was enabled, but Clapper version requirement is not met'.format(name))
  endif
  subdir_done()
endif

enhancer_plugin_template = 'clapper-media-scanner.plugin.in'

enhancer_deps += [
  dependency('gstreamer-1.0', version: '>= 1.20.0', required: false),
  dependency('gstreamer-tag-1.0', version: '>= 1.20.0', required: false),
]
enhancer_sources += [
  'media-scanner/clapper-media-scanner.c',
]
