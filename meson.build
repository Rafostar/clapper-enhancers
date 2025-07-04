project('clapper-enhancers', 'c',
  version: '0.8.2', # major and minor numbers indicate required libclapper version
  meson_version: '>= 0.64.0',
  license: 'LGPL-2.1-or-later',
  default_options: [
    'warning_level=1',
    'buildtype=debugoptimized',
  ],
)

enhancers_version = meson.project_version().split('-')[0]
version_array = enhancers_version.split('.')

glib_req = '>= 2.76.0'
clapper_req = '>= @0@.@1@'.format(version_array[0], version_array[1])

clapper_api_name = 'clapper-@0@.0'.format(version_array[0])

optimization = get_option('optimization')
build_optimized = optimization in ['2', '3', 's']

glib_dep = dependency('glib-2.0',
  version: glib_req,
  required: false,
)
gobject_dep = dependency('gobject-2.0',
  version: glib_req,
  required: false,
)
gio_dep = dependency('gio-2.0',
  version: glib_req,
  required: false,
)
gmodule_dep = dependency('gmodule-2.0',
  version: glib_req,
  required: false,
)
peas_dep = dependency('libpeas-2',
  required: false,
)
clapper_dep = dependency('clapper-0.0',
  version: clapper_req,
  required: false,
)

# Unless custom dir is set, enhancers dir is determined from libclapper compilation
clapper_enhancers_dir = get_option('enhancersdir').strip()
if clapper_enhancers_dir == ''
  clapper_enhancers_dir = clapper_dep.get_variable('enhancersdir')
endif

cc = meson.get_compiler('c')

warning_flags = [
  '-Wmissing-declarations',
  '-Wredundant-decls',
  '-Wwrite-strings',
  '-Wformat',
  '-Wformat-security',
  '-Winit-self',
  '-Wmissing-include-dirs',
  '-Waddress',
  '-Wno-multichar',
  '-Wvla',
  '-Wpointer-arith',
  '-Wmissing-prototypes',
  '-Wdeclaration-after-statement',
  '-Wold-style-definition',
  '-Wsign-compare',
]

foreach extra_arg : warning_flags
  if cc.has_argument (extra_arg)
    add_project_arguments([extra_arg], language: 'c')
  endif
endforeach

if build_optimized
  message('Disabling GLib cast checks')
  add_project_arguments('-DG_DISABLE_CAST_CHECKS', language: 'c')

  message('Disabling GLib asserts')
  add_project_arguments('-DG_DISABLE_ASSERT', language: 'c')

  message('Disabling GLib checks')
  add_project_arguments('-DG_DISABLE_CHECKS', language: 'c')
endif

subdir('src')

summary({
  'enhancers-dir': clapper_enhancers_dir,
  'optimization': optimization,
}, section: 'Directories')

foreach name : clapper_possible_enhancers
  summary(name, clapper_available_enhancers.contains(name) ? 'Yes' : 'No', section: 'Enhancers')
endforeach
