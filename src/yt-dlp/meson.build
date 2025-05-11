enhancer_plugin_template = 'clapper_yt_dlp.plugin.in'

enhancer_data += [
  'yt-dlp/clapper_yt_dlp.py',
  'yt-dlp/clapper_yt_dlp_overrides.py',
  'yt-dlp/clapper_yt_dlp_dash.py',
  'yt-dlp/clapper_yt_dlp_hls.py',
  'yt-dlp/clapper_yt_dlp_direct.py',
]

# Enhancer code has Clapper version checks
enhancer_configurable = (clapper_dep.found() and clapper_dep.version().version_compare('>= 0.9.0'))
