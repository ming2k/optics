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

# Liquid Glass SPIR-V
liquid_glass_comp_spv = custom_target('liquid_glass.comp.spv',
  input   : 'src/shaders/liquid_glass.comp',
  output  : 'liquid_glass.comp.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.3', '@INPUT@', '-o', '@OUTPUT@'],
)
liquid_glass_16f_comp_spv = custom_target('liquid_glass_16f.comp.spv',
  input   : 'src/shaders/liquid_glass.comp',
  output  : 'liquid_glass_16f.comp.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.3', '-DPRISM_STORAGE_RGBA16F',
             '@INPUT@', '-o', '@OUTPUT@'],
)

# Frosted Glass SPIR-V
frosted_comp_spv = custom_target('frosted.comp.spv',
  input   : 'src/shaders/frosted.comp',
  output  : 'frosted.comp.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.3', '@INPUT@', '-o', '@OUTPUT@'],
)
frosted_16f_comp_spv = custom_target('frosted_16f.comp.spv',
  input   : 'src/shaders/frosted.comp',
  output  : 'frosted_16f.comp.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.3', '-DPRISM_STORAGE_RGBA16F',
             '@INPUT@', '-o', '@OUTPUT@'],
)

# Acrylic SPIR-V
acrylic_comp_spv = custom_target('acrylic.comp.spv',
  input   : 'src/shaders/acrylic.comp',
  output  : 'acrylic.comp.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.3', '@INPUT@', '-o', '@OUTPUT@'],
)
acrylic_16f_comp_spv = custom_target('acrylic_16f.comp.spv',
  input   : 'src/shaders/acrylic.comp',
  output  : 'acrylic_16f.comp.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.3', '-DPRISM_STORAGE_RGBA16F',
             '@INPUT@', '-o', '@OUTPUT@'],
)

# Common utility SPIR-V
storage_clear_comp_spv = custom_target('storage_clear.comp.spv',
  input   : 'src/shaders/storage_clear.comp',
  output  : 'storage_clear.comp.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.3', '@INPUT@', '-o', '@OUTPUT@'],
)
storage_clear_16f_comp_spv = custom_target('storage_clear_16f.comp.spv',
  input   : 'src/shaders/storage_clear.comp',
  output  : 'storage_clear_16f.comp.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.3', '-DPRISM_STORAGE_RGBA16F',
             '@INPUT@', '-o', '@OUTPUT@'],
)
backdrop_stats_comp_spv = custom_target('backdrop_stats.comp.spv',
  input   : 'src/shaders/backdrop_stats.comp',
  output  : 'backdrop_stats.comp.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.3', '@INPUT@', '-o', '@OUTPUT@'],
)

prism_c_args = ['-DPRISM_BUILDING=1']
prism_c_args += cc.get_supported_arguments('--embed-dir=' + meson.current_build_dir())

spv2h_py = import('python').find_installation('python3')
spv2h = files('../../tools/spv2h.py')

# Headers for fallback (no-#embed)
liquid_glass_spv_h = custom_target('liquid_glass_spv.h',
  input   : liquid_glass_comp_spv,
  output  : 'liquid_glass_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'liquid_glass_spv'],
)
liquid_glass_16f_spv_h = custom_target('liquid_glass_16f_spv.h',
  input   : liquid_glass_16f_comp_spv,
  output  : 'liquid_glass_16f_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'liquid_glass_16f_spv'],
)

frosted_spv_h = custom_target('frosted_spv.h',
  input   : frosted_comp_spv,
  output  : 'frosted_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'frosted_spv'],
)
frosted_16f_spv_h = custom_target('frosted_16f_spv.h',
  input   : frosted_16f_comp_spv,
  output  : 'frosted_16f_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'frosted_16f_spv'],
)

acrylic_spv_h = custom_target('acrylic_spv.h',
  input   : acrylic_comp_spv,
  output  : 'acrylic_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'acrylic_spv'],
)
acrylic_16f_spv_h = custom_target('acrylic_16f_spv.h',
  input   : acrylic_16f_comp_spv,
  output  : 'acrylic_16f_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'acrylic_16f_spv'],
)

backdrop_frost_comp_spv = custom_target('backdrop_frost.comp.spv',
  input   : 'src/shaders/backdrop_frost.comp',
  output  : 'backdrop_frost.comp.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.3', '@INPUT@', '-o', '@OUTPUT@'],
)
backdrop_frost_16f_comp_spv = custom_target('backdrop_frost_16f.comp.spv',
  input   : 'src/shaders/backdrop_frost.comp',
  output  : 'backdrop_frost_16f.comp.spv',
  command : [glslang, '-V', '--target-env', 'vulkan1.3', '-DPRISM_STORAGE_RGBA16F',
             '@INPUT@', '-o', '@OUTPUT@'],
)

storage_clear_spv_h = custom_target('storage_clear_spv.h',
  input   : storage_clear_comp_spv,
  output  : 'storage_clear_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'storage_clear_spv'],
)
storage_clear_16f_spv_h = custom_target('storage_clear_16f_spv.h',
  input   : storage_clear_16f_comp_spv,
  output  : 'storage_clear_16f_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'storage_clear_16f_spv'],
)
backdrop_stats_spv_h = custom_target('backdrop_stats_spv.h',
  input   : backdrop_stats_comp_spv,
  output  : 'backdrop_stats_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'backdrop_stats_spv'],
)
backdrop_frost_spv_h = custom_target('backdrop_frost_spv.h',
  input : backdrop_frost_comp_spv,
  output : 'backdrop_frost_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'backdrop_frost_spv'],
)
backdrop_frost_16f_spv_h = custom_target('backdrop_frost_16f_spv.h',
  input : backdrop_frost_16f_comp_spv,
  output : 'backdrop_frost_16f_spv.h',
  command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', 'backdrop_frost_16f_spv'],
)

prism_lib = library('prism',
  'src/liquid_glass.c',
  'src/frosted.c',
  'src/acrylic.c',
  'src/backdrop_layer.c',
  'src/version.c',
  liquid_glass_comp_spv, liquid_glass_16f_comp_spv,
  frosted_comp_spv, frosted_16f_comp_spv,
  acrylic_comp_spv, acrylic_16f_comp_spv,
  backdrop_frost_comp_spv, backdrop_frost_16f_comp_spv,
  storage_clear_comp_spv, storage_clear_16f_comp_spv, backdrop_stats_comp_spv,
  liquid_glass_spv_h, liquid_glass_16f_spv_h,
  frosted_spv_h, frosted_16f_spv_h,
  acrylic_spv_h, acrylic_16f_spv_h,
  backdrop_frost_spv_h, backdrop_frost_16f_spv_h,
  storage_clear_spv_h, storage_clear_16f_spv_h, backdrop_stats_spv_h,
  include_directories   : prism_inc,
  c_args                : prism_c_args,
  dependencies          : [flux_dep, m_dep],
  gnu_symbol_visibility : 'hidden',
  soversion             : optics_soname,
  version               : meson.project_version(),
  install               : true,
)

install_headers(
  'include/prism/prism.h',
  'include/prism/types.h',
  'include/prism/liquid_glass.h',
  'include/prism/frosted.h',
  'include/prism/acrylic.h',
  'include/prism/backdrop_layer.h',
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
  description : 'Optics material library (liquid glass, frosted, acrylic, layered backdrop), built on the flux effect runtime',
  version     : meson.project_version(),
  requires    : ['flux >= ' + meson.project_version()],
)
