json_utils_dep = dependency('', required: false)

utils_deps = [
  glib_dep,
  gio_dep,
  clapper_dep,
  dependency('json-glib-1.0', version: '>=1.2.0', required: false)
]
foreach dep : utils_deps
  if not dep.found()
    if utils_option.enabled()
      error('@0@ utils were enabled, but required dependencies were not found'.format(name))
    endif
    subdir_done()
  endif
endforeach

utils_sources = [
  'json-utils.c',
]

json_utils_dep = declare_dependency(
  link_with: static_library(
    'clapper-enhancers-@0@-utils'.format(name),
    utils_sources,
    dependencies: utils_deps,
    c_args: utils_c_args,
  ),
  dependencies: utils_deps,
)
