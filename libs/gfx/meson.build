# liboptics_gfx — the rendering foundation: Vulkan 1.3 RHI, 2D canvas (GPU and CPU
# backends), compute pipelines, image effects, the analytic curve kernel, and
# distance fields.
#
# gfx is one library with one ABI. There are no per-module build switches:
# every installed liboptics_gfx exports the same symbols and ships the same headers,
# so "which gfx is this?" is answered by the version alone.

vulkan_dep  = dependency('vulkan',  version : '>=1.3', required : true)
threads_dep = dependency('threads')

gfx_inc = [include_directories('include'), include_directories('.')]

# dma-buf (fd-based external memory import/export + sync_file fences) is
# Linux-only. Elsewhere src/core/dmabuf_stub.c answers every public entry point
# with OPTICS_GFX_ERROR_UNSUPPORTED, so call sites need no #ifdef (ADR-0052).
have_dmabuf = host_machine.system() == 'linux'

gfx_sources = files(
  'src/core/device.c',
  'src/core/surface.c',
  'src/core/memory.c',
  'src/core/oneshot.c',
  'src/core/frame.c',
  'src/core/damage_history.c',
  'src/core/plan.c',
  'src/core/render_plan.c',
  'src/core/result.c',
  'src/core/version.c',
  'src/core/log.c',
  'src/core/format.c',
  'src/core/sampler.c',
  'src/core/buffer.c',
  'src/core/target.c',
  'src/core/graphics_pipeline.c',
  'src/core/vk_allocator.c',
  'src/core/observe.c',
  'src/core/profile.c',
  'src/core/icc.c',
  'src/math/vec.c',
  'src/math/mat.c',
  'src/math/quat.c',
  'src/math/color.c',
  'src/math/colorspace.c',
  'src/math/arena.c',
  'src/curve/curve.c',
  'src/sdf/sdf.c',
  'src/canvas/image.c',
  'src/canvas/canvas.c',
  'src/canvas/canvas_dispatch.c',
  'src/canvas/path.c',
  'src/canvas/paint.c',
  'src/canvas/geometry_stroke.c',
  'src/canvas/curve_flatten.c',
  'src/canvas/coverage_raster.c',
  'src/canvas/canvas_tile.c',
  'src/canvas/renderer.c',
  'src/canvas/record.c',
  'src/canvas/backend_vk.c',
  'src/canvas/backend_cpu.c',
  'src/canvas/dmabuf_acquire.c',
  'src/canvas/encoder.c',
  'src/compute/compute.c',
  'src/effect/effect.c',
)
if have_dmabuf
  gfx_sources += files('src/core/dmabuf.c')
else
  gfx_sources += files('src/core/dmabuf_stub.c')
endif

gfx_c_args = ['-DOPTICS_GFX_BUILDING=1']
gfx_public_defines = []
if have_dmabuf
  gfx_c_args += '-DOPTICS_GFX_HAVE_DMABUF=1'
  gfx_public_defines += '-DOPTICS_GFX_HAVE_DMABUF=1'
endif

# Observation-plane attribution (ADR-0129). Off by default so a release build
# carries no name/site machinery.
if get_option('observe_attribution')
  gfx_c_args += '-DOPTICS_GFX_OBSERVE_ATTRIBUTION=1'
endif

# C23 #embed bakes each compiled .spv into the binary (renderer.c, effect.c).
# --embed-dir points the compiler at the build directory.
gfx_c_args += '--embed-dir=' + meson.current_build_dir()

# ---------------------------------------------------------------------------
# Shaders (Slang -> SPIR-V, ADR-0156). One table, one rule.
# ---------------------------------------------------------------------------
slangc = find_program('slangc', required : true)
slangc_flags = ['-target', 'spirv', '-profile', 'spirv_1_6', '-Wno-41012']

gfx_shader_inc = [
  '-I' + meson.current_source_dir() / 'src/canvas/shaders',
  '-I' + meson.current_source_dir() / 'include',
  '-I' + meson.current_source_dir() / 'include/optics/gfx/shaders',
]
# Shared snippets: editing any of them rebuilds every shader.
optics_gfx_shader_deps = files(
  'src/canvas/shaders/canvas_color.slang',
  'src/canvas/shaders/canvas_image_common.slang',
  'src/canvas/shaders/canvas_tile_common.slang',
  'include/optics/gfx/shaders/optics_gfx_sdf.slang',
  'include/optics/gfx/shaders/optics_gfx_vector_eval.slang',
)
optics_gfx_stage = {'vert' : 'vertex', 'frag' : 'fragment', 'comp' : 'compute'}

# [directory, source stem, stage, output stem, extra slangc args]
optics_gfx_shaders = [
  ['canvas', 'canvas_solid',       'vert', 'canvas_solid',           []],
  ['canvas', 'canvas_solid',       'frag', 'canvas_solid',           []],
  ['canvas', 'canvas_gradient',    'frag', 'canvas_gradient',        []],
  ['canvas', 'canvas_image',       'frag', 'canvas_image',           []],
  ['canvas', 'canvas_glyph',       'frag', 'canvas_glyph',           []],
  ['canvas', 'canvas_sdf',         'frag', 'canvas_sdf',             []],
  ['canvas', 'canvas_output',      'vert', 'canvas_output',          []],
  ['canvas', 'canvas_output',      'frag', 'canvas_output',          []],
  ['canvas', 'canvas_tile_raster', 'comp', 'canvas_tile_raster',     []],
  ['canvas', 'canvas_tile_bin',    'comp', 'canvas_tile_bin_count',  []],
  ['canvas', 'canvas_tile_bin',    'comp', 'canvas_tile_bin_scatter',['-DFOR_SCATTER=1']],
  ['canvas', 'canvas_tile_scan',   'comp', 'canvas_tile_scan_offsets', []],
  ['canvas', 'canvas_tile_scan',   'comp', 'canvas_tile_scan_backdrop', ['-DFOR_BACKDROP=1']],
  ['canvas', 'canvas_vector',      'frag', 'canvas_vector',          []],
  ['effect', 'effect_blur',        'comp', 'effect_blur',         []],
  ['effect', 'effect_backdrop',    'comp', 'effect_backdrop',     []],
  ['effect', 'effect_shadow',      'comp', 'effect_shadow',       []],
  # ADR-0069: rgba16f storage variants for working-space (16F) content.
  ['effect', 'effect_blur',        'comp', 'effect_blur_16f',     ['-DEFFECT_STORAGE_RGBA16F']],
  ['effect', 'effect_backdrop',    'comp', 'effect_backdrop_16f', ['-DEFFECT_STORAGE_RGBA16F']],
  ['effect', 'effect_shadow',      'comp', 'effect_shadow_16f',   ['-DEFFECT_STORAGE_RGBA16F']],
]

optics_gfx_spv = {}
optics_gfx_extra_inputs = []
foreach s : optics_gfx_shaders
  out = '@0@.@1@.spv'.format(s[3], s[2])
  spv = custom_target(out,
    input        : 'src/@0@/shaders/@1@.@2@.slang'.format(s[0], s[1], s[2]),
    depend_files : optics_gfx_shader_deps,
    output       : out,
    command      : [slangc, slangc_flags, '-entry', 'main', '-stage', optics_gfx_stage[s[2]],
                    s[4], gfx_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  optics_gfx_spv += {out : spv}
  optics_gfx_extra_inputs += spv
endforeach
# Consumed by the tile-raster integration tests.
canvas_tile_raster_comp_spv = optics_gfx_spv['canvas_tile_raster.comp.spv']

# SONAME policy: `optics_soname` is derived once in the top-level meson.build
# (full version pre-1.0, major.minor after); `version` is always X.Y.Z.
gfx_lib = library('optics_gfx',
  gfx_sources,
  optics_gfx_extra_inputs,
  include_directories   : gfx_inc,
  c_args                : gfx_c_args,
  dependencies          : [vulkan_dep, threads_dep, m_dep, tracy_dep],
  gnu_symbol_visibility : 'hidden',
  soversion             : optics_soname,
  version               : meson.project_version(),
  install               : true,
)

install_headers('include/optics/gfx.h', subdir : 'optics')
install_headers(
  'include/optics/gfx/core.h',
  'include/optics/gfx/math.h',
  'include/optics/gfx/curve.h',
  'include/optics/gfx/sdf.h',
  'include/optics/gfx/vulkan.h',
  'include/optics/gfx/observe.h',
  'include/optics/gfx/observe_sites.h',
  'include/optics/gfx/profile.h',
  'include/optics/gfx/dmabuf.h',
  'include/optics/gfx/canvas.h',
  'include/optics/gfx/canvas_helpers.h',
  'include/optics/gfx/canvas_cpu.h',
  'include/optics/gfx/compute.h',
  'include/optics/gfx/effect.h',
  subdir : 'optics/gfx',
)

# The analytic distance field and curve evaluation kernels are installed for
# external Slang shader consumers too (ADR-0118, ADR-0156).
install_headers('include/optics/gfx/shaders/optics_gfx_sdf.slang',
                'include/optics/gfx/shaders/optics_gfx_vector_eval.slang',
                subdir : 'optics/gfx/shaders')

gfx_dep = declare_dependency(
  link_with             : gfx_lib,
  include_directories   : gfx_inc,
  compile_args          : gfx_public_defines,
  dependencies          : [vulkan_dep, tracy_dep],
)

pkg.generate(
  libraries   : gfx_lib,
  name        : 'optics-gfx',
  filebase    : 'optics-gfx',
  description  : 'C23 Vulkan-first 2D graphics library (RHI, canvas, compute, effect)',
  version      : meson.project_version(),
  extra_cflags : gfx_public_defines,
)
