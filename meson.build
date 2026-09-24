project('optics', ['c'],
  version : '0.0.48',
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

# Fuzz targets and the ci-fuzz workflow must run under clang (libFuzzer
# runtime is clang-only). Probe with the clang executable directly —
# `cc` here is whatever CC meson picked (often gcc, which rejects
# -fsanitize=fuzzer even when clang and its runtime are installed).
clang_cc = find_program('clang', required : false, version : '>=10')

# Soname policy shared by every library in this tree (see
# docs/reference/api.md): the SONAME encodes the compatibility boundary
# (major.minor while pre-1.0 — patches are binary compatible, minors may
# break), while the real-name suffix carries the full X.Y.Z. Deriving it
# once here keeps all six libraries consistent by construction.
optics_version_parts = meson.project_version().split('.')
optics_soname = '@0@.@1@'.format(optics_version_parts[0], optics_version_parts[1])

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

if get_option('anim')
  subdir('libs/anim')
else
  # Always defined so tests/examples can reference it unconditionally; empty
  # when the library is off.
  anim_dep = declare_dependency()
endif

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
  if get_option('anim')
    subdir('examples/anim')
  endif
  subdir('examples/lens')
  subdir('examples/iris')
  subdir('examples/scenarios')
endif

# LeakSanitizer suppressions (fontconfig's process-lifetime pattern cache
# is upstream-owned and never freed). Default test setup when ASan is on.
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
  if clang_cc.found()
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
  subdir('tests/anim')
  subdir('tests/lens')
  subdir('tests/iris')
endif
