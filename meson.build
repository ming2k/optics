project('optics', ['c'],
  version : '0.0.36',
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
  subdir('examples/lens')
  subdir('examples/iris')
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
  # The probe compiles AND links a trivial harness so the check fails
  # whenever the runtime is absent, instead of poisoning every fuzz
  # executable's link line. `libfuzzer_enabled` is read by
  # tests/flux/fuzz and tests/lens/fuzz to pick the driver: libFuzzer's
  # main() replaces fuzz_main.c's standalone driver.
  #
  # NOTE: b_sanitize must NOT already contain 'fuzzer' here; the probe
  # would double-enable it and fail spuriously. meson forbids
  # b_sanitize=fuzzer together with other sanitizers, and the fuzz CI
  # job configures with b_sanitize=address explicitly.
  libfuzzer_c_args = ['-fsanitize=fuzzer-no-link']
  libfuzzer_link_args = []
  if cc.links(
    'int LLVMFuzzerTestOneInput(const unsigned char *d, unsigned long s);\n'
    + 'int main(void) { return LLVMFuzzerTestOneInput("x", 1) != 0; }\n',
    args : libfuzzer_c_args + ['-fsanitize=fuzzer'],
    name : 'libFuzzer runtime (clang -fsanitize=fuzzer)',
  )
    libfuzzer_enabled = true
    libfuzzer_link_args = ['-fsanitize=fuzzer']
  else
    libfuzzer_enabled = false
    # Standalone build: keep -fsanitize=fuzzer-no-link off the compile
    # line so plain gcc/clang builds of the harnesses stay warning-clean.
    libfuzzer_c_args = []
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
