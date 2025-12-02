# Enhancers
clapper_possible_enhancers = [
  'control-hub',
  'lbry',
  'media-scanner',
  'mpris',
  'parser-m3u',
  'peertube',
  'recall',
  'yt-dlp',
]

subdir('utils')

clapper_available_enhancers = []
clapper_configurable_enhancers = []

foreach name : clapper_possible_enhancers
  enhancer_option = get_option(name)
  if not enhancer_option.disabled()
    enhancer_deps = []
    enhancer_data = []
    enhancer_sources = []
    enhancer_plugin_template = ''
    enhancer_configurable = false

    split_name_arr = name.split('-')
    name_upper = ''
    foreach split_name : split_name_arr
      name_upper += split_name.substring(0, 1).to_upper() + split_name.substring(1)
    endforeach

    enhancer_c_args = ['-DG_LOG_DOMAIN="Clapper@0@"'.format(name_upper)]
    enhancer_install_dir = join_paths(clapper_enhancers_dir, name)

    subdir(name)
    inc_dir = include_directories(name)

    # Common boilerplate for any enhancer that has sources to be build
    if enhancer_sources.length() > 0 or enhancer_configurable
      enhancer_deps += [glib_dep, gio_dep, clapper_dep]
    endif
    if enhancer_sources.length() > 0
      enhancer_deps += [gobject_dep, gmodule_dep, peas_dep]
    endif

    can_build = true
    foreach dep : enhancer_deps
      if not dep.found()
        if enhancer_option.enabled()
          error('@0@ enhancer was enabled, but required dependencies were not found'.format(name))
        endif
        can_build = false
      endif
    endforeach

    enhancer_available = false
    if can_build
      if enhancer_data.length() > 0
        install_data(
          enhancer_data,
          install_dir: enhancer_install_dir,
        )
        enhancer_available = true
      endif
      if enhancer_sources.length() > 0
        library(
          'clapper-@0@'.format(name),
          enhancer_sources,
          dependencies: enhancer_deps,
          include_directories: inc_dir,
          c_args: enhancer_c_args,
          install: true,
          install_dir: enhancer_install_dir,
        )
        enhancer_available = true
      endif
      if enhancer_available
        if enhancer_plugin_template != ''
          py = import('python').find_installation()
          custom_target(enhancer_plugin_template,
            input: join_paths(name, enhancer_plugin_template),
            output: enhancer_plugin_template.replace('.in', ''),
            command: [py, files('gen-version.py'), '@INPUT@', meson.project_version(), get_option('enhancersver')],
            capture: true,
            build_by_default: true,
            build_always_stale: true,
            install: true,
            install_dir: enhancer_install_dir,
          )
        endif
        clapper_available_enhancers += name
        if enhancer_configurable
          clapper_configurable_enhancers += name
        endif
      endif
    endif
  endif
endforeach

if clapper_configurable_enhancers.length() > 0
  clapper_functionalities = clapper_dep.get_variable('functionalities').split()

  if not (clapper_functionalities.contains('enhancers-loader'))
    error('Cannot compile schemas without Clapper built with "enhancers-loader"')
  endif

  # Used inside "gen-schemas" executable
  find_program('glib-compile-schemas', required: true)

  gen_schemas_bin = executable(
    'gen-schemas',
    ['gen-schemas.c'],
    dependencies: [glib_dep, gio_dep, clapper_dep],
    install: false,
  )
  meson.add_install_script(gen_schemas_bin, clapper_enhancers_dir, ','.join(clapper_configurable_enhancers))
endif
