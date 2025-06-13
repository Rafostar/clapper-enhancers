if not clapper_dep.found() or not clapper_dep.version().version_compare('>= 0.9.0')
  if enhancer_option.enabled()
    error('@0@ enhancer was enabled, but Clapper version requirement is not met'.format(name))
  endif
  subdir_done()
endif

enhancer_plugin_template = 'clapper-recall.plugin.in'

enhancer_deps += [
  dependency('sqlite3', required: false),
]
enhancer_sources += [
  'recall/clapper-recall.c',
]
enhancer_configurable = true
