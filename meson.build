project('optics', ['c'],
  version : '0.0.8',
  license : 'MIT',
  meson_version : '>=1.8.0',
  default_options : [
    'c_std=c23',
    'warning_level=3',
    'default_library=shared',
    'b_ndebug=if-release',
  ],
)

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required : false)
pkg = import('pkgconfig')

add_project_arguments(
  '-D_GNU_SOURCE',
  '-Wno-unused-parameter',
  '-Wno-missing-field-initializers',
  language : 'c',
)

subdir('libs/flux')


subdir('libs/lens')
subdir('libs/iris')

if get_option('examples')
  subdir('examples/flux')
  if get_option('text')
    subdir('examples/flux-text')
  endif
  if get_option('scene-graph')
    subdir('examples/flux-scene-graph')
  endif
  subdir('examples/lens')
  subdir('examples/iris')
endif

if get_option('tests')
  subdir('tests/flux')
  subdir('tests/lens')
  subdir('tests/iris')
endif
