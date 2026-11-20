project('optics', ['c'],
  version : '0.0.82',
  license : 'MIT',
  meson_version : '>=1.8.0',
  default_options : [
    'c_std=c23',
    'warning_level=3',
    'default_library=both',
    'b_ndebug=if-release',
  ],
)

cc = meson.get_compiler('c')
m_dep = cc.find_library('m', required : false)
pkg = import('pkgconfig')

# Fuzz targets and the ci-fuzz workflow must run under clang (libFuzzer
# runtime is clang-only). Probe with the clang executable directly —
# `cc` here is whatever CC meson picked (often gcc, which rejects
# -fsanitize=fuzzer even when clang and its runtime are installed).
clang_cc = find_program('clang', required : false, version : '>=10')

# Soname policy shared by every library in this tree (see
# docs/reference/api.md).
#
# Pre-1.0 there is no ABI promise: this tree has repeatedly appended fields to
# tagged descriptor structs and changed behavior in *patch* releases
# (`0.0.74 -> 0.0.75` added the Canvas damage scope). A SONAME of
# `major.minor` collapses every `0.0.x` release onto `libflux.so.0.0`, so a
# binary built against one release silently loads another and reads a struct
# whose layout it does not share. The pre-1.0 compatibility boundary is
# therefore the full version string. From 1.0 on it is `major.minor`, while the
# real-name suffix continues to carry the full X.Y.Z.
optics_version_parts = meson.project_version().split('.')
if optics_version_parts[0].to_int() == 0
  optics_soname = meson.project_version()
else
  optics_soname = '@0@.@1@'.format(optics_version_parts[0], optics_version_parts[1])
endif

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

opt_tracy = get_option('tracy')
if opt_tracy
  add_languages('cpp', required : true, native : false)
  tracy_dep = dependency('tracy', fallback : ['tracy', 'tracy_dep'], required : true)
  add_project_arguments('-DOPTICS_PROFILER_TRACY=1', language : 'c')
else
  tracy_dep = declare_dependency()
endif

opt_text = get_option('text')
opt_scene = get_option('scene')
opt_motion = get_option('motion')
opt_svg = get_option('svg')
opt_material = get_option('material')
# lens consumes glyph and reticle. The feature graph is explicit here so a
# disabled dependency cannot turn into a late undefined reference.
opt_ui = opt_text and opt_svg

subdir('libs/gfx')

if opt_text
  subdir('libs/text')
else
  text_dep = declare_dependency()
endif

if opt_scene
  subdir('libs/scene')
else
  scene_dep = declare_dependency()
endif

if opt_motion
  subdir('libs/motion')
else
  motion_dep = declare_dependency()
endif

if opt_svg
  subdir('libs/svg')
else
  svg_dep = declare_dependency()
endif

if opt_material
  subdir('libs/material')
else
  # Always defined so tests/examples can reference it unconditionally; empty
  # when the library is off.
  material_dep = declare_dependency()
endif

# iris is the platform shell and needs flux only. lens is the UI engine and
# needs glyph + reticle. aperture is the one library that links both.
subdir('libs/window')

if opt_ui
  subdir('libs/ui')
  subdir('libs/app')
else
  ui_dep = declare_dependency()
  app_dep = declare_dependency()
endif

# ====================================================================
# Optics Umbrella Facade & Dependencies (ADR-0160)
# ====================================================================
optics_inc = include_directories('include')

optics_deps = [gfx_dep, window_dep]
optics_requires = ['optics-gfx = ' + meson.project_version(), 'optics-window = ' + meson.project_version()]
optics_compile_args = []

if opt_motion
  optics_deps += [motion_dep]
  optics_requires += ['optics-motion = ' + meson.project_version()]
  optics_compile_args += '-DOPTICS_HAVE_MOTION=1'
endif

if opt_text
  optics_deps += [text_dep]
  optics_requires += ['optics-text = ' + meson.project_version()]
  optics_compile_args += '-DOPTICS_HAVE_TEXT=1'
endif
if opt_scene
  optics_deps += [scene_dep]
  optics_requires += ['optics-scene = ' + meson.project_version()]
  optics_compile_args += '-DOPTICS_HAVE_SCENE=1'
endif
if opt_svg
  optics_deps += [svg_dep]
  optics_requires += ['optics-svg = ' + meson.project_version()]
  optics_compile_args += '-DOPTICS_HAVE_SVG=1'
endif
if opt_material
  optics_deps += [material_dep]
  optics_requires += ['optics-material = ' + meson.project_version()]
  optics_compile_args += '-DOPTICS_HAVE_MATERIAL=1'
endif
if opt_ui
  optics_deps += [ui_dep, app_dep]
  optics_requires += [
    'optics-ui = ' + meson.project_version(),
    'optics-app = ' + meson.project_version(),
  ]
  optics_compile_args += ['-DOPTICS_HAVE_UI=1', '-DOPTICS_HAVE_APP=1']
endif

install_headers('include/optics/optics.h', subdir : 'optics')

optics_dep = declare_dependency(
  include_directories : optics_inc,
  dependencies        : optics_deps,
  compile_args        : optics_compile_args,
)

pkg.generate(
  libraries    : optics_dep,
  name         : 'optics',
  filebase     : 'optics',
  description  : 'High-performance C23 graphics and UI stack (umbrella facade)',
  version      : meson.project_version(),
  requires     : optics_requires,
  extra_cflags : optics_compile_args,
)

if get_option('examples')
  subdir('examples/getting_started')
  subdir('examples/showcase')
endif

if get_option('fuzz')
  # Single point of truth for libFuzzer availability. The engine needs
  # its runtime linked; the -fsanitize=fuzzer compile flag alone is not
  # enough (a binary that links without -fsanitize=fuzzer aborts at
  # startup). Both mainline clang and Apple clang provide the runtime;
  # GCC does not (its -fsanitize=fuzzer is not implemented).
  #
  # The probe is what the fuzz executables actually are: a complete
  # program with main() (libFuzzer's driver ships one) that references
  # LLVMFuzzerTestOneInput, linked with -fsanitize=fuzzer. Probed with
  # clang directly (see clang_cc above): `cc` may be gcc, which rejects
  # the flag regardless of whether the clang runtime is installed.
  libfuzzer_c_args = []
  libfuzzer_link_args = []
  libfuzzer_enabled = false
  if clang_cc.found() and cc.get_id() == 'clang'
    # Compile+link a complete fuzz program with clang itself.
    probe_src = '/tmp/.optics_libfuzzer_probe.c'
    run_command('sh', '-c',
      'cat > ' + probe_src + ' <<"EOF"\n'
      + 'int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long s);\n'
      + 'int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long s) {\n'
      + '  (void)d; (void)s; return 0;\n'
      + '}\nEOF', check : false)
    probe_ok = run_command(clang_cc, '-fsanitize=fuzzer', probe_src,
                           '-o', '/tmp/.optics_libfuzzer_probe', check : false)
    if probe_ok.returncode() == 0
      libfuzzer_enabled = true
      libfuzzer_c_args = ['-fsanitize=fuzzer-no-link']
      libfuzzer_link_args = ['-fsanitize=fuzzer']
      message('libFuzzer runtime: OK (clang) — fuzz executables link the engine')
    else
      message('libFuzzer runtime unavailable (' + probe_ok.stderr().strip() + ') — standalone harnesses')
    endif
  else
    message('clang not found — libFuzzer unavailable, standalone harnesses')
  endif
endif

# Sanitizer test setup: suppresses known process-lifetime allocations
# (upstream fontconfig pattern cache) and unifies diagnostic aborts,
# stack traces, and execution timeouts across all sanitizer suites.
san_opts = get_option('b_sanitize')
if san_opts != 'none' and san_opts != ''
  san_env = environment()

  if 'address' in san_opts or 'leak' in san_opts
    san_env.prepend('LSAN_OPTIONS', 'suppressions=' + meson.project_source_root() / 'tools' / 'config' / 'lsan.supp', separator : ':')
  endif

  if 'address' in san_opts
    san_env.prepend('ASAN_OPTIONS', 'abort_on_error=1:halt_on_error=0', separator : ':')
  endif

  if 'undefined' in san_opts
    san_env.prepend('UBSAN_OPTIONS', 'print_stacktrace=1', separator : ':')
  endif

  if 'thread' in san_opts
    san_env.prepend('TSAN_OPTIONS', 'abort_on_error=1:halt_on_error=1', separator : ':')
  endif

  if 'memory' in san_opts
    san_env.prepend('MSAN_OPTIONS', 'abort_on_error=1:halt_on_error=1', separator : ':')
  endif

  add_test_setup(
    'sanitizer',
    env : san_env,
    timeout_multiplier : 4,
    is_default : true,
  )
endif

if get_option('tests')
  # One orchestrator owns the test topology and the shared support layer.
  subdir('tests')
endif
