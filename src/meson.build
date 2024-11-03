# Enhancers
clapper_possible_enhancers = [
  'yt-dlp',
]

subdir('utils')

clapper_available_enhancers = []
foreach name : clapper_possible_enhancers
  enhancer_option = get_option(name)
  if not enhancer_option.disabled()
    enhancer_deps = [clapper_dep]
    subdir(name)
  endif
endforeach
