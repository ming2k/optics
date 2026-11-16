# libflux — the rendering foundation: Vulkan 1.3 RHI, 2D canvas (GPU and CPU
# backends), 3D draw core, compute pipelines, image effects, the analytic curve
# kernel and distance fields.
#
# flux is one library with one ABI. There are no per-module build switches:
# every installed libflux exports the same symbols and ships the same headers,
# so "which flux is this?" is answered by the version alone.

vulkan_dep  = dependency('vulkan',  version : '>=1.3', required : true)
threads_dep = dependency('threads')

flux_inc = [include_directories('include'), include_directories('.')]

# dma-buf (fd-based external memory import/export + sync_file fences) is
# Linux-only. Elsewhere src/core/dmabuf_stub.c answers every public entry point
# with FLUX_ERROR_UNSUPPORTED, so call sites need no #ifdef (ADR-0052).
have_dmabuf = host_machine.system() == 'linux'

flux_sources = files(
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
  'src/vault/tess_earcut.c',
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
  'src/scene/scene.c',
  'src/compute/compute.c',
  'src/effect/effect.c',
)
if have_dmabuf
  flux_sources += files('src/core/dmabuf.c')
else
  flux_sources += files('src/core/dmabuf_stub.c')
endif

flux_c_args = ['-DFLUX_BUILDING=1']
flux_public_defines = []
if have_dmabuf
  flux_c_args += '-DFLUX_HAVE_DMABUF=1'
  flux_public_defines += '-DFLUX_HAVE_DMABUF=1'
endif

# Observation-plane attribution (ADR-0129). Off by default so a release build
# carries no name/site machinery.
if get_option('observe_attribution')
  flux_c_args += '-DFLUX_OBSERVE_ATTRIBUTION=1'
endif

# C23 #embed bakes each compiled .spv into the binary (renderer.c, effect.c,
# scene.c). --embed-dir points the compiler at the build directory.
flux_c_args += cc.get_supported_arguments('--embed-dir=' + meson.current_build_dir())

# ---------------------------------------------------------------------------
# Shaders (Slang -> SPIR-V, ADR-0156). One table, one rule.
# ---------------------------------------------------------------------------
slangc = find_program('slangc', required : true)
slangc_flags = ['-target', 'spirv', '-profile', 'spirv_1_6', '-Wno-41012']

flux_shader_inc = [
  '-I' + meson.current_source_dir() / 'src/canvas/shaders',
  '-I' + meson.current_source_dir() / 'src/scene/shaders',
  '-I' + meson.current_source_dir() / 'include',
  '-I' + meson.current_source_dir() / 'include/flux/shaders',
]
# Shared snippets: editing any of them rebuilds every shader.
flux_shader_deps = files(
  'src/canvas/shaders/canvas_color.slang',
  'src/canvas/shaders/canvas_image_common.slang',
  'src/canvas/shaders/canvas_tile_common.slang',
  'src/scene/shaders/scene_color.slang',
  'include/flux/shaders/flux_sdf.slang',
  'include/flux/shaders/flux_vector_eval.slang',
)
flux_stage = {'vert' : 'vertex', 'frag' : 'fragment', 'comp' : 'compute'}

# [directory, source stem, stage, output stem, extra slangc args]
flux_shaders = [
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
  ['scene',  'scene_unlit',        'vert', 'scene_unlit',         []],
  ['scene',  'scene_unlit',        'frag', 'scene_unlit',         []],
  ['scene',  'scene_phong',        'vert', 'scene_phong',         []],
  ['scene',  'scene_phong',        'frag', 'scene_phong',         []],
  ['scene',  'scene_unlit_skin',   'vert', 'scene_unlit_skin',    []],
  ['scene',  'scene_phong_skin',   'vert', 'scene_phong_skin',    []],
  ['scene',  'scene_vector',       'vert', 'scene_vector',        []],
  ['scene',  'scene_vector',       'frag', 'scene_vector',        []],
  ['effect', 'effect_blur',        'comp', 'effect_blur',         []],
  ['effect', 'effect_backdrop',    'comp', 'effect_backdrop',     []],
  ['effect', 'effect_shadow',      'comp', 'effect_shadow',       []],
  # ADR-0069: rgba16f storage variants for working-space (16F) content.
  ['effect', 'effect_blur',        'comp', 'effect_blur_16f',     ['-DEFFECT_STORAGE_RGBA16F']],
  ['effect', 'effect_backdrop',    'comp', 'effect_backdrop_16f', ['-DEFFECT_STORAGE_RGBA16F']],
  ['effect', 'effect_shadow',      'comp', 'effect_shadow_16f',   ['-DEFFECT_STORAGE_RGBA16F']],
]

flux_spv = {}
flux_extra_inputs = []
foreach s : flux_shaders
  out = '@0@.@1@.spv'.format(s[3], s[2])
  spv = custom_target(out,
    input        : 'src/@0@/shaders/@1@.@2@.slang'.format(s[0], s[1], s[2]),
    depend_files : flux_shader_deps,
    output       : out,
    command      : [slangc, slangc_flags, '-entry', 'main', '-stage', flux_stage[s[2]],
                    s[4], flux_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  flux_spv += {out : spv}
  flux_extra_inputs += spv
endforeach
# Consumed by the tile-raster integration tests.
canvas_tile_raster_comp_spv = flux_spv['canvas_tile_raster.comp.spv']

# SONAME policy: `optics_soname` is derived once in the top-level meson.build
# (full version pre-1.0, major.minor after); `version` is always X.Y.Z.
flux_lib = library('flux',
  flux_sources,
  flux_extra_inputs,
  include_directories   : flux_inc,
  c_args                : flux_c_args,
  dependencies          : [vulkan_dep, threads_dep, m_dep, tracy_dep],
  gnu_symbol_visibility : 'hidden',
  soversion             : optics_soname,
  version               : meson.project_version(),
  install               : true,
)

install_headers(
  'include/flux/flux.h',
  'include/flux/core.h',
  'include/flux/math.h',
  'include/flux/curve.h',
  'include/flux/sdf.h',
  'include/flux/vulkan.h',
  'include/flux/observe.h',
  'include/flux/profile.h',
  'include/flux/dmabuf.h',
  'include/flux/canvas.h',
  'include/flux/canvas_helpers.h',
  'include/flux/canvas_cpu.h',
  'include/flux/scene.h',
  'include/flux/compute.h',
  'include/flux/effect.h',
  subdir : 'flux',
)

# The analytic distance field is defined once for Slang consumers too
# (ADR-0118, ADR-0156); prism's materials include it rather than keeping copies.
install_headers('include/flux/shaders/flux_sdf.slang',
                'include/flux/shaders/flux_vector_eval.slang',
                subdir : 'flux/shaders')
install_headers('include/flux/vault/tess_earcut.h', subdir : 'flux/vault')

flux_dep = declare_dependency(
  link_with             : flux_lib,
  include_directories   : flux_inc,
  compile_args          : flux_public_defines,
  dependencies          : [vulkan_dep, tracy_dep],
)

pkg.generate(
  libraries   : flux_lib,
  name        : 'flux',
  filebase    : 'flux',
  description  : 'C23 Vulkan-first graphics library (canvas, scene, compute, effect)',
  version      : meson.project_version(),
  extra_cflags : flux_public_defines,
)
