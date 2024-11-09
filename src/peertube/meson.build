enhancer_deps += [
  dependency('gstreamer-1.0', version: '>= 1.20.0', required: false),
  dependency('gstreamer-tag-1.0', version: '>= 1.20.0', required: false),
  dependency('libsoup-3.0', required: false),
  common_utils_dep,
  json_utils_dep,
]
foreach dep : enhancer_deps
  if not dep.found()
    if enhancer_option.enabled()
      error('@0@ enhancer was enabled, but required dependencies were not found'.format(name))
    endif
    subdir_done()
  endif
endforeach

enhancer_data = [
  'clapper-peertube.plugin',
]
enhancer_sources = [
  'clapper-peertube.c',
]

library(
  'clapper-peertube',
  enhancer_sources,
  dependencies: enhancer_deps,
  c_args: enhancer_c_args,
  install: true,
  install_dir: enhancer_install_dir,
)
install_data(
  enhancer_data,
  install_dir: enhancer_install_dir,
)
clapper_available_enhancers += name
