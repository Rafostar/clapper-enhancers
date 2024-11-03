# Utils
all_c_utils = [
  'common',
]

foreach name : all_c_utils
  name_upper = name.substring(0, 1).to_upper() + name.substring(1)
  utils_c_args = ['-DG_LOG_DOMAIN="ClapperEnhancersUtils@0@"'.format(name_upper)]
  subdir(name)
endforeach
