# prism — the material library of the optics stack. Named materials (their
# shaders, curve shapes, and parameter contracts) live here, built on flux's
# public effect runtime: compute pipelines, compute-writable images, frame
# slots, and the bindless heap. flux owns rendering mechanism, prism owns
# material identity, and the caller owns policy (ADR-0063).

if not get_option('compute')
  error('prism requires the flux compute module — pass -Dcompute=true with -Dprism=true')
endif

prism_inc = include_directories('include')

glslang = find_program('glslangValidator', required : true)
liquid_glass_comp_spv = custom_target('liquid_glass.comp.spv',
  input   : 'src/shaders/liquid_glass.comp',
  output  : 'liquid_glass.comp.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.3', '@INPUT@', '-o', '@OUTPUT@'],
)
storage_clear_comp_spv = custom_target('storage_clear.comp.spv',
  input   : 'src/shaders/storage_clear.comp',
  output  : 'storage_clear.comp.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.3', '@INPUT@', '-o', '@OUTPUT@'],
)

prism_c_args = ['-DPRISM_BUILDING=1']
# C23 #embed bakes each compiled .spv into the binary (src/liquid_glass.c).
# Compilers without #embed (MSVC cl, Apple Clang) take the generated-header
# path instead — same idiom as libs/flux. --embed-dir is probed because
# MSVC rejects unknown options outright.
prism_c_args += cc.get_supported_arguments('--embed-dir=' + meson.current_build_dir())

# Generated shader headers for the no-#embed fallback. The C source picks
# per translation unit via __has_embed, so the headers must exist
# regardless of host compiler.
spv2h_py = import('python').find_installation('python3')
spv2h = files('../../tools/spv2h.py')
liquid_glass_spv_h = custom_target('liquid_glass_spv.h',
  input   : liquid_glass_comp_spv,
  output  : 'liquid_glass_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'liquid_glass_spv'],
)
storage_clear_spv_h = custom_target('storage_clear_spv.h',
  input   : storage_clear_comp_spv,
  output  : 'storage_clear_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'storage_clear_spv'],
)

prism_lib = library('prism',
  'src/liquid_glass.c',
  liquid_glass_comp_spv, storage_clear_comp_spv,
  liquid_glass_spv_h, storage_clear_spv_h,
  include_directories   : prism_inc,
  c_args                : prism_c_args,
  dependencies          : [flux_dep, m_dep],
  gnu_symbol_visibility : 'hidden',
  version               : meson.project_version(),
  install               : true,
)

install_headers(
  'include/prism/prism.h',
  'include/prism/liquid_glass.h',
  subdir : 'prism',
)

prism_dep = declare_dependency(
  link_with           : prism_lib,
  include_directories : prism_inc,
  dependencies        : [flux_dep],
)

pkg.generate(
  libraries   : prism_lib,
  name        : 'prism',
  filebase    : 'prism',
  description : 'Optics material library (liquid glass), built on the flux effect runtime',
  version     : meson.project_version(),
  requires    : ['flux >= ' + meson.project_version()],
)
