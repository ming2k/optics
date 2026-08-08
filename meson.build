project('optics', ['c'],
  version : '0.0.11',
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

# _GNU_SOURCE is a glibc feature-test macro; defining it on Windows /
# macOS toolchains is at best meaningless and at worst an error.
if host_machine.system() == 'linux'
  add_project_arguments('-D_GNU_SOURCE', language : 'c')
endif
add_project_arguments(
  # -Wno-* spellings are GCC/Clang-only; probe so MSVC cl (which errors
  # on unknown options) can configure the tree.
  cc.get_supported_arguments('-Wno-unused-parameter', '-Wno-missing-field-initializers'),
  language : 'c',
)

subdir('libs/flux')


subdir('libs/lens')
subdir('libs/iris')

if get_option('examples')
  subdir('examples/flux')
  subdir('examples/showcase')
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
