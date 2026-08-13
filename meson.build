project('xtream-player', 'c', version: '0.1.0', default_options: ['c_std=c11', 'warning_level=2'])

dependencies = [
  dependency('gtk4', version: '>=4.10'),
  dependency('libadwaita-1', version: '>=1.4'),
  dependency('libsoup-3.0'),
  dependency('json-glib-1.0'),
  dependency('gstreamer-1.0'),
]

executable('xtream-player', 'src/main.c',
  dependencies: dependencies,
  install: true,
)

install_data('data/io.github.mackilanu.XtreamPlayer.desktop',
  install_dir: get_option('datadir') / 'applications')
