project('music-player', 'c',
  version : '1.0',
  license : 'MIT',
  default_options : ['c_std=gnu11', 'warning_level=2', 'buildtype=release', 'b_lto=true'])

add_project_arguments('-march=native', '-Wno-unused-parameter', language : 'c')

gtk = dependency('gtk+-3.0')
mpv = dependency('mpv')
avformat = dependency('libavformat')
avcodec = dependency('libavcodec')
avutil = dependency('libavutil')
swscale = dependency('libswscale')
sqlite = dependency('sqlite3')
x11 = dependency('x11')
m = meson.get_compiler('c').find_library('m')

core = files('src/track.c', 'src/format.c', 'src/tags.c', 'src/cue.c', 'src/library.c')

executable('music-player',
  core + files('src/player.c', 'src/icons.c', 'src/plview.c', 'src/libview.c', 'src/dialogs.c', 'src/ctl.c', 'src/main.c'),
  dependencies : [gtk, mpv, avformat, avcodec, avutil, swscale, sqlite, x11, m],
  install : true)

executable('music-player-ctl', 'src/music-player-ctl.c', install : true)

# Tests: the title formatter, tags, cue sheets and the library, without a window.
test_deps = [dependency('glib-2.0'), dependency('gdk-pixbuf-2.0'), avformat, avcodec, avutil, swscale, sqlite, m]
test('format', executable('test-format', core + files('tests/test_format.c'), dependencies : test_deps))
test('library', executable('test-library', core + files('tests/test_library.c'), dependencies : test_deps),
  env : ['TESTDATA=' + meson.current_build_dir() / 'testdata'], timeout : 120)
test('playback', executable('test-playback', core + files('src/player.c', 'tests/test_playback.c'),
  dependencies : test_deps + [mpv]),
  env : ['TESTDATA=' + meson.current_build_dir() / 'testdata'], timeout : 180)
