project('optics', ['c'],
  version : '0.0.14',
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

if get_option('prism')
  subdir('libs/prism')
else
  # Always defined so tests/examples can reference it unconditionally; empty
  # when the library is off.
  prism_dep = declare_dependency()
endif

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

# LeakSanitizer suppressions (fontconfig's process-lifetime pattern cache
# is upstream-owned and never freed). Default test setup when ASan is on.
if 'address' in get_option('b_sanitize')
  add_test_setup(
    'asan',
    env: {'LSAN_OPTIONS': 'suppressions=' + meson.project_source_root() / 'tools' / 'lsan.supp'},
    is_default: true,
  )
endif

if get_option('tests')
  subdir('tests/flux')
  subdir('tests/prism')
  subdir('tests/lens')
  subdir('tests/iris')
endif
