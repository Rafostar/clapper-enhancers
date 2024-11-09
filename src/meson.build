# Enhancers
clapper_possible_enhancers = [
  'peertube',
  'yt-dlp',
]

subdir('utils')

clapper_available_enhancers = []
foreach name : clapper_possible_enhancers
  enhancer_option = get_option(name)
  if not enhancer_option.disabled()
    split_name_arr = name.split('-')
    name_upper = ''
    foreach split_name : split_name_arr
      name_upper += split_name.substring(0, 1).to_upper() + split_name.substring(1)
    endforeach
    enhancer_deps = [glib_dep, gobject_dep, gio_dep, gmodule_dep, peas_dep, clapper_dep]
    enhancer_c_args = ['-DG_LOG_DOMAIN="Clapper@0@"'.format(name_upper)]
    enhancer_install_dir = join_paths(clapper_enhancers_dir, name)
    subdir(name)
  endif
endforeach
