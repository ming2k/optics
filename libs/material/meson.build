# prism — the material library of the optics stack. Named materials (their
# shaders, curve shapes, and parameter contracts) live here, built on flux's
# public effect runtime: compute pipelines, compute-writable images, frame
# slots, and the bindless heap. flux owns rendering mechanism, prism owns
# material identity, and the caller owns policy (ADR-0063).

material_inc = include_directories('include')

slangc = find_program('slangc', required : true)
slangc_flags = ['-target', 'spirv', '-profile', 'spirv_1_6', '-Wno-41012']

# The analytic distance field is defined once, in flux (ADR-0118); Prism's
# materials consume that definition instead of carrying their own copies.
flux_sdf_inc = [
  '-I' + meson.project_source_root() / 'libs/gfx/include',
  '-I' + meson.project_source_root() / 'libs/gfx/include/optics/gfx/shaders',
]
flux_sdf_slang = files(meson.project_source_root() / 'libs/gfx/include/optics/gfx/shaders/optics_gfx_sdf.slang')

# Liquid Glass SPIR-V
liquid_glass_comp_spv = custom_target('liquid_glass.comp.spv',
  input   : 'src/shaders/liquid_glass.comp.slang',
  depend_files : flux_sdf_slang,
  output  : 'liquid_glass.comp.spv',
  command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
             flux_sdf_inc, '@INPUT@', '-o', '@OUTPUT@'],
)
liquid_glass_16f_comp_spv = custom_target('liquid_glass_16f.comp.spv',
  input   : 'src/shaders/liquid_glass.comp.slang',
  depend_files : flux_sdf_slang,
  output  : 'liquid_glass_16f.comp.spv',
  command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
             flux_sdf_inc, '-DOPTICS_MATERIAL_STORAGE_RGBA16F', '@INPUT@', '-o', '@OUTPUT@'],
)

# Frosted Glass SPIR-V
frosted_comp_spv = custom_target('frosted.comp.spv',
  input   : 'src/shaders/frosted.comp.slang',
  depend_files : flux_sdf_slang,
  output  : 'frosted.comp.spv',
  command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
             flux_sdf_inc, '@INPUT@', '-o', '@OUTPUT@'],
)
frosted_16f_comp_spv = custom_target('frosted_16f.comp.spv',
  input   : 'src/shaders/frosted.comp.slang',
  depend_files : flux_sdf_slang,
  output  : 'frosted_16f.comp.spv',
  command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
             flux_sdf_inc, '-DOPTICS_MATERIAL_STORAGE_RGBA16F', '@INPUT@', '-o', '@OUTPUT@'],
)

# Acrylic SPIR-V
acrylic_comp_spv = custom_target('acrylic.comp.spv',
  input   : 'src/shaders/acrylic.comp.slang',
  depend_files : flux_sdf_slang,
  output  : 'acrylic.comp.spv',
  command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
             flux_sdf_inc, '@INPUT@', '-o', '@OUTPUT@'],
)
acrylic_16f_comp_spv = custom_target('acrylic_16f.comp.spv',
  input   : 'src/shaders/acrylic.comp.slang',
  depend_files : flux_sdf_slang,
  output  : 'acrylic_16f.comp.spv',
  command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
             flux_sdf_inc, '-DOPTICS_MATERIAL_STORAGE_RGBA16F', '@INPUT@', '-o', '@OUTPUT@'],
)

# Mica SPIR-V
mica_comp_spv = custom_target('mica.comp.spv',
  input   : 'src/shaders/mica.comp.slang',
  depend_files : flux_sdf_slang,
  output  : 'mica.comp.spv',
  command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
             flux_sdf_inc, '@INPUT@', '-o', '@OUTPUT@'],
)
mica_16f_comp_spv = custom_target('mica_16f.comp.spv',
  input   : 'src/shaders/mica.comp.slang',
  depend_files : flux_sdf_slang,
  output  : 'mica_16f.comp.spv',
  command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
             flux_sdf_inc, '-DOPTICS_MATERIAL_STORAGE_RGBA16F', '@INPUT@', '-o', '@OUTPUT@'],
)

# Common utility SPIR-V
storage_clear_comp_spv = custom_target('storage_clear.comp.spv',
  input   : 'src/shaders/storage_clear.comp.slang',
  depend_files : flux_sdf_slang,
  output  : 'storage_clear.comp.spv',
  command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
             flux_sdf_inc, '@INPUT@', '-o', '@OUTPUT@'],
)
storage_clear_16f_comp_spv = custom_target('storage_clear_16f.comp.spv',
  input   : 'src/shaders/storage_clear.comp.slang',
  depend_files : flux_sdf_slang,
  output  : 'storage_clear_16f.comp.spv',
  command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
             flux_sdf_inc, '-DOPTICS_MATERIAL_STORAGE_RGBA16F', '@INPUT@', '-o', '@OUTPUT@'],
)
backdrop_stats_comp_spv = custom_target('backdrop_stats.comp.spv',
  input   : 'src/shaders/backdrop_stats.comp.slang',
  depend_files : flux_sdf_slang,
  output  : 'backdrop_stats.comp.spv',
  command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
             flux_sdf_inc, '@INPUT@', '-o', '@OUTPUT@'],
)

material_c_args = ['-DOPTICS_MATERIAL_BUILDING=1']
material_c_args += cc.get_supported_arguments('--embed-dir=' + meson.current_build_dir())

backdrop_frost_comp_spv = custom_target('backdrop_frost.comp.spv',
  input   : 'src/shaders/backdrop_frost.comp.slang',
  depend_files : flux_sdf_slang,
  output  : 'backdrop_frost.comp.spv',
  command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
             flux_sdf_inc, '@INPUT@', '-o', '@OUTPUT@'],
)
backdrop_frost_16f_comp_spv = custom_target('backdrop_frost_16f.comp.spv',
  input   : 'src/shaders/backdrop_frost.comp.slang',
  depend_files : flux_sdf_slang,
  output  : 'backdrop_frost_16f.comp.spv',
  command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
             flux_sdf_inc, '-DOPTICS_MATERIAL_STORAGE_RGBA16F', '@INPUT@', '-o', '@OUTPUT@'],
)

material_lib = library('optics_material',
  'src/liquid_glass.c',
  'src/frosted.c',
  'src/acrylic.c',
  'src/mica.c',
  'src/backdrop_layer.c',
  'src/version.c',
  liquid_glass_comp_spv, liquid_glass_16f_comp_spv,
  frosted_comp_spv, frosted_16f_comp_spv,
  acrylic_comp_spv, acrylic_16f_comp_spv,
  mica_comp_spv, mica_16f_comp_spv,
  backdrop_frost_comp_spv, backdrop_frost_16f_comp_spv,
  storage_clear_comp_spv, storage_clear_16f_comp_spv, backdrop_stats_comp_spv,
  include_directories   : material_inc,
  c_args                : material_c_args,
  dependencies          : [gfx_dep, m_dep],
  gnu_symbol_visibility : 'hidden',
  soversion             : optics_soname,
  version               : meson.project_version(),
  install               : true,
)

install_headers('include/optics/material.h', subdir : 'optics')
install_headers(
    'include/optics/material/types.h',
  'include/optics/material/liquid_glass.h',
  'include/optics/material/frosted.h',
  'include/optics/material/acrylic.h',
  'include/optics/material/mica.h',
  'include/optics/material/backdrop_layer.h',
  subdir : 'optics/material',
)

material_dep = declare_dependency(
  link_with           : material_lib,
  include_directories : material_inc,
  dependencies        : [gfx_dep],
)

pkg.generate(
  libraries   : material_lib,
  name        : 'optics-material',
  filebase    : 'optics-material',
  description : 'Optics material library (liquid glass, frosted, acrylic, mica, layered backdrop), built on the flux effect runtime',
  version     : meson.project_version(),
  requires    : ['optics-gfx >= ' + meson.project_version()],
)
