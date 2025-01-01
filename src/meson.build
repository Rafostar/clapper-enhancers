# Enhancers
clapper_possible_enhancers = [
  'lbry',
  'peertube',
  'yt-dlp',
]

subdir('utils')

clapper_available_enhancers = []
foreach name : clapper_possible_enhancers
  enhancer_option = get_option(name)
  if not enhancer_option.disabled()
    enhancer_deps = []
    enhancer_data = []
    enhancer_sources = []

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
    if enhancer_sources.length() > 0
      enhancer_deps += [glib_dep, gobject_dep, gio_dep, gmodule_dep, peas_dep, clapper_dep]
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

    if can_build
      if enhancer_data.length() > 0
        install_data(
          enhancer_data,
          install_dir: enhancer_install_dir,
        )
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
      endif
      clapper_available_enhancers += name
    endif
  endif
endforeach
