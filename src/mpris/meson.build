# Known OSes that can support our MPRIS implementation
os_supported = ['linux'].contains(host_machine.system())

if not os_supported
  if enhancer_option.enabled()
    error('@0@ enhancer was enabled, but OS is not supported by it'.format(name))
  endif
  subdir_done()
endif

if not clapper_dep.found() or not clapper_dep.version().version_compare('>= 0.9.1')
  if enhancer_option.enabled()
    error('@0@ enhancer was enabled, but Clapper version requirement is not met'.format(name))
  endif
  subdir_done()
endif

gdbus_codegen = find_program('gdbus-codegen', required: false)
if not gdbus_codegen.found()
  if enhancer_option.enabled()
    error('@0@ enhancer was enabled, but program gdbus-codegen was not found'.format(name))
  endif
  subdir_done()
endif

gnome = import('gnome')

clapper_mpris_gdbus = gnome.gdbus_codegen('clapper-mpris-gdbus',
  sources: 'clapper-mpris-gdbus.xml',
  interface_prefix: 'org.mpris.',
  namespace: 'ClapperEnhancerMpris', # FIXME: 1.0: Rename to ClapperMpris
)

enhancer_plugin_template = 'clapper-mpris.plugin.in'

enhancer_deps += [
  dependency('gio-unix-2.0', version: glib_req, required: false),
  dependency('gstreamer-1.0', version: '>= 1.20.0', required: false),
  dependency('gstreamer-tag-1.0', version: '>= 1.20.0', required: false),
]
enhancer_sources += [
  'mpris/clapper-mpris.c',
  clapper_mpris_gdbus
]
