if not clapper_dep.found() or not clapper_dep.version().version_compare('>= 0.10.0')
  if enhancer_option.enabled()
    error('@0@ enhancer was enabled, but Clapper version requirement is not met'.format(name))
  endif
  subdir_done()
endif

enhancer_plugin_template = 'clapper-dir-walker.plugin.in'

enhancer_sources += [
  'dir-walker/clapper-dir-walker.c',
]
enhancer_configurable = true
