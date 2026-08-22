# Single first-party library: libflux. The C source tree sits at the repo
# root (`src/`, `include/`, `tests/`, `examples/`). Content layers that feed
# flux's draw primitives live in `libs/` as siblings (ADR-0016): `flux-text`
# (HarfBuzz shaping, feeds flux_canvas_draw_glyph_run) and `flux-scene-graph`
# (glTF, feeds flux_scene_draw_mesh). md is removed (out of scope). Rust
# bindings live in the separate flux-rs repository.

vulkan_dep  = dependency('vulkan',  version : '>=1.3', required : true)
threads_dep = dependency('threads')

opt_canvas  = get_option('canvas')
opt_scene   = get_option('scene')
opt_compute = get_option('compute')
opt_effect  = get_option('effect')

if opt_effect and not opt_canvas
  error('effect module requires canvas (flux_image) — pass -Dcanvas=true with -Deffect=true')
endif
if opt_effect and not opt_compute
  error('effect module requires compute (flux_compute_pipeline) — pass -Dcompute=true with -Deffect=true')
endif
if get_option('text') and not opt_canvas
  error('text sibling requires canvas (flux_canvas_draw_glyph_run) — pass -Dcanvas=true with -Dtext=true')
endif

flux_inc = include_directories('include')

# dma-buf (fd-based external memory import/export + sync_file fences) is
# Linux-only. When off, src/core/dmabuf_stub.c replaces dmabuf.c and
# answers every public entry point FLUX_ERROR_UNSUPPORTED. The sources are
# core-level, NOT part of the canvas module gate: device.c / surface.c /
# frame.c call the acquire-semaphore pool helpers and flux_dmabuf_supported
# unconditionally (ADR-0052: no #ifdef at call sites), so a -Dcanvas=false
# build must still link them. The one canvas-internal entry point lives in
# src/canvas/dmabuf_acquire.c inside the canvas gate.
have_dmabuf = host_machine.system() == 'linux'
if have_dmabuf
  flux_c_args_dma = ['-DFLUX_HAVE_DMABUF=1']
else
  flux_c_args_dma = []
endif

flux_sources = files(
  'src/core/device.c',
  'src/core/surface.c',
  'src/core/memory.c',
  'src/core/oneshot.c',
  'src/core/frame.c',
  'src/core/result.c',
  'src/core/version.c',
  'src/core/log.c',
  'src/core/format.c',
  'src/core/sampler.c',
  'src/core/buffer.c',
  'src/core/target.c',
  'src/core/graphics_pipeline.c',
  'src/core/vk_allocator.c',
  'src/core/icc.c',
  'src/canvas/image.c',
  'src/math/vec.c',
  'src/math/mat.c',
  'src/math/quat.c',
  'src/math/color.c',
  'src/math/colorspace.c',
  'src/math/arena.c',
)
if have_dmabuf
  flux_sources += files('src/core/dmabuf.c')
else
  flux_sources += files('src/core/dmabuf_stub.c')
endif
flux_extra_inputs = []

flux_c_args = ['-DFLUX_BUILDING=1'] + flux_c_args_dma

# C23 #embed bakes each compiled .spv into the binary (renderer.c /
# effect.c / scene.c). Compilers without #embed (MSVC cl, Apple Clang)
# take a generated-header path instead — the headers are produced from
# the same .spv files by tools/spv2h.py further below. --embed-dir is
# probed because MSVC rejects unknown options outright.
flux_embed_dir_arg = cc.get_supported_arguments('--embed-dir=' + meson.current_build_dir())
if opt_canvas or opt_scene or opt_effect
  flux_c_args += flux_embed_dir_arg
endif
# Per-shader accumulators for the generated-header fallback; the
# custom_target loop sits after the module blocks.
flux_shader_spvs = []
flux_shader_names = []
if opt_canvas
  flux_sources += files(
    'src/canvas/canvas.c',
    'src/canvas/path.c',
    'src/canvas/paint.c',
    'src/canvas/geometry_flatten.c',
    'src/canvas/geometry_tess.c',
    'src/canvas/geometry_stroke.c',
    'src/canvas/renderer.c',
    'src/canvas/record.c',
    'src/canvas/backend_vk.c',
    'src/canvas/backend_cpu.c',
    'src/canvas/dmabuf_acquire.c',
  )
  flux_c_args += '-DFLUX_HAVE_CANVAS=1'

  glslang = find_program('glslangValidator', required : true)
  canvas_solid_vert_spv = custom_target('canvas_solid.vert.spv',
    input   : 'src/canvas/shaders/canvas_solid.vert',
    depend_files : 'src/canvas/shaders/canvas_color.glsl',

    output  : 'canvas_solid.vert.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.3',
               '-I' + meson.current_source_dir() / 'src/canvas/shaders',
               '@INPUT@', '-o', '@OUTPUT@'],
  )
  canvas_solid_frag_spv = custom_target('canvas_solid.frag.spv',
    input   : 'src/canvas/shaders/canvas_solid.frag',
    output  : 'canvas_solid.frag.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.3',
               '-I' + meson.current_source_dir() / 'src/canvas/shaders',
               '@INPUT@', '-o', '@OUTPUT@'],
  )
  canvas_gradient_frag_spv = custom_target('canvas_gradient.frag.spv',
    input   : 'src/canvas/shaders/canvas_gradient.frag',
    depend_files : 'src/canvas/shaders/canvas_color.glsl',

    output  : 'canvas_gradient.frag.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.3',
               '-I' + meson.current_source_dir() / 'src/canvas/shaders',
               '@INPUT@', '-o', '@OUTPUT@'],
  )
  canvas_image_frag_spv = custom_target('canvas_image.frag.spv',
    input   : 'src/canvas/shaders/canvas_image.frag',
    depend_files : 'src/canvas/shaders/canvas_color.glsl',

    output  : 'canvas_image.frag.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.3',
               '-I' + meson.current_source_dir() / 'src/canvas/shaders',
               '@INPUT@', '-o', '@OUTPUT@'],
  )
  canvas_glyph_frag_spv = custom_target('canvas_glyph.frag.spv',
    input   : 'src/canvas/shaders/canvas_glyph.frag',
    output  : 'canvas_glyph.frag.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.3',
               '-I' + meson.current_source_dir() / 'src/canvas/shaders',
               '@INPUT@', '-o', '@OUTPUT@'],
  )
  canvas_sdf_frag_spv = custom_target('canvas_sdf.frag.spv',
    input   : 'src/canvas/shaders/canvas_sdf.frag',
    output  : 'canvas_sdf.frag.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.3',
               '-I' + meson.current_source_dir() / 'src/canvas/shaders',
               '@INPUT@', '-o', '@OUTPUT@'],
  )
  canvas_output_vert_spv = custom_target('canvas_output.vert.spv',
    input   : 'src/canvas/shaders/canvas_output.vert',
    output  : 'canvas_output.vert.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.3',
               '-I' + meson.current_source_dir() / 'src/canvas/shaders',
               '@INPUT@', '-o', '@OUTPUT@'],
  )
  canvas_output_frag_spv = custom_target('canvas_output.frag.spv',
    input   : 'src/canvas/shaders/canvas_output.frag',
    depend_files : 'src/canvas/shaders/canvas_color.glsl',

    output  : 'canvas_output.frag.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.3',
               '-I' + meson.current_source_dir() / 'src/canvas/shaders',
               '@INPUT@', '-o', '@OUTPUT@'],
  )
  flux_extra_inputs += [canvas_solid_vert_spv, canvas_solid_frag_spv,
                        canvas_gradient_frag_spv, canvas_image_frag_spv,
                        canvas_glyph_frag_spv, canvas_sdf_frag_spv,
                        canvas_output_vert_spv, canvas_output_frag_spv]
  flux_shader_spvs += [canvas_solid_vert_spv, canvas_solid_frag_spv,
                       canvas_gradient_frag_spv, canvas_image_frag_spv,
                       canvas_glyph_frag_spv, canvas_sdf_frag_spv,
                       canvas_output_vert_spv, canvas_output_frag_spv]
  flux_shader_names += ['canvas_solid_vert_spv', 'canvas_solid_frag_spv',
                        'canvas_gradient_frag_spv', 'canvas_image_frag_spv',
                        'canvas_glyph_frag_spv', 'canvas_sdf_frag_spv',
                        'canvas_output_vert_spv', 'canvas_output_frag_spv']
endif
if opt_scene
  flux_sources += files('src/scene/scene.c')
  flux_c_args += '-DFLUX_HAVE_SCENE=1'

  if not is_variable('glslang')
    glslang = find_program('glslangValidator', required : true)
  endif
  scene_shader_deps = ['src/scene/shaders/scene_color.glsl',
                       'src/canvas/shaders/canvas_color.glsl']
  scene_shader_inc = ['-I' + meson.current_source_dir() / 'src/canvas/shaders',
                      '-I' + meson.current_source_dir() / 'src/scene/shaders']
  scene_unlit_vert_spv = custom_target('scene_unlit.vert.spv',
    input   : 'src/scene/shaders/scene_unlit.vert',
    depend_files : scene_shader_deps,
    output  : 'scene_unlit.vert.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.3',
               scene_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  scene_unlit_frag_spv = custom_target('scene_unlit.frag.spv',
    input   : 'src/scene/shaders/scene_unlit.frag',
    depend_files : scene_shader_deps,
    output  : 'scene_unlit.frag.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.3',
               scene_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  scene_phong_vert_spv = custom_target('scene_phong.vert.spv',
    input   : 'src/scene/shaders/scene_phong.vert',
    depend_files : scene_shader_deps,
    output  : 'scene_phong.vert.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.3',
               scene_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  scene_phong_frag_spv = custom_target('scene_phong.frag.spv',
    input   : 'src/scene/shaders/scene_phong.frag',
    depend_files : scene_shader_deps,
    output  : 'scene_phong.frag.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.3',
               scene_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  scene_unlit_skin_vert_spv = custom_target('scene_unlit_skin.vert.spv',
    input   : 'src/scene/shaders/scene_unlit_skin.vert',
    depend_files : scene_shader_deps,
    output  : 'scene_unlit_skin.vert.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.3',
               scene_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  scene_phong_skin_vert_spv = custom_target('scene_phong_skin.vert.spv',
    input   : 'src/scene/shaders/scene_phong_skin.vert',
    depend_files : scene_shader_deps,
    output  : 'scene_phong_skin.vert.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.3',
               scene_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  flux_extra_inputs += [scene_unlit_vert_spv, scene_unlit_frag_spv,
                        scene_phong_vert_spv, scene_phong_frag_spv,
                        scene_unlit_skin_vert_spv, scene_phong_skin_vert_spv]
  flux_shader_spvs += [scene_unlit_vert_spv, scene_unlit_frag_spv,
                       scene_phong_vert_spv, scene_phong_frag_spv,
                       scene_unlit_skin_vert_spv, scene_phong_skin_vert_spv]
  flux_shader_names += ['scene_unlit_vert_spv', 'scene_unlit_frag_spv',
                        'scene_phong_vert_spv', 'scene_phong_frag_spv',
                        'scene_unlit_skin_vert_spv', 'scene_phong_skin_vert_spv']
endif
if opt_compute
  flux_sources += files('src/compute/compute.c')
  flux_c_args += '-DFLUX_HAVE_COMPUTE=1'
endif
if opt_effect
  flux_sources += files('src/effect/effect.c')
  flux_c_args += '-DFLUX_HAVE_EFFECT=1'

  if not is_variable('glslang')
    glslang = find_program('glslangValidator', required : true)
  endif
  effect_blur_comp_spv = custom_target('effect_blur.comp.spv',
    input   : 'src/effect/shaders/effect_blur.comp',
    output  : 'effect_blur.comp.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.3',
               '-I' + meson.current_source_dir() / 'src/canvas/shaders',
               '@INPUT@', '-o', '@OUTPUT@'],
  )
  effect_backdrop_comp_spv = custom_target('effect_backdrop.comp.spv',
    input   : 'src/effect/shaders/effect_backdrop.comp',
    output  : 'effect_backdrop.comp.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.3',
               '-I' + meson.current_source_dir() / 'src/canvas/shaders',
               '@INPUT@', '-o', '@OUTPUT@'],
  )
  # ADR-0069: rgba16f storage variants for working-space (16F) content.
  effect_blur_16f_comp_spv = custom_target('effect_blur_16f.comp.spv',
    input   : 'src/effect/shaders/effect_blur.comp',
    output  : 'effect_blur_16f.comp.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.3', '-DEFFECT_STORAGE_RGBA16F',
               '-I' + meson.current_source_dir() / 'src/canvas/shaders',
               '@INPUT@', '-o', '@OUTPUT@'],
  )
  effect_backdrop_16f_comp_spv = custom_target('effect_backdrop_16f.comp.spv',
    input   : 'src/effect/shaders/effect_backdrop.comp',
    output  : 'effect_backdrop_16f.comp.spv',
    command : [glslang, '-V', '--target-env', 'vulkan1.3', '-DEFFECT_STORAGE_RGBA16F',
               '-I' + meson.current_source_dir() / 'src/canvas/shaders',
               '@INPUT@', '-o', '@OUTPUT@'],
  )
  flux_extra_inputs += [effect_blur_comp_spv, effect_backdrop_comp_spv,
                        effect_blur_16f_comp_spv, effect_backdrop_16f_comp_spv]
  flux_shader_spvs += [effect_blur_comp_spv, effect_backdrop_comp_spv,
                       effect_blur_16f_comp_spv, effect_backdrop_16f_comp_spv]
  flux_shader_names += ['effect_blur_spv', 'effect_backdrop_spv',
                        'effect_blur_16f_spv', 'effect_backdrop_16f_spv']
endif

# Generated shader headers for the no-#embed fallback (MSVC cl / Apple
# Clang). The C sources pick per translation unit via __has_embed, so the
# headers must exist regardless of host compiler; each is one small
# script run over the matching .spv.
if flux_shader_spvs.length() > 0
  spv2h_py = import('python').find_installation('python3')
  spv2h = files('../../tools/spv2h.py')
  foreach i : range(flux_shader_spvs.length())
    flux_extra_inputs += custom_target(flux_shader_names[i] + '.h',
      input   : flux_shader_spvs[i],
      output  : flux_shader_names[i] + '.h',
      command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', flux_shader_names[i]],
    )
  endforeach
  # ccache does not hash C23 #embed payloads (observed 4.13.6): without
  # this, editing a shader silently reuses the previous object. The
  # manifest header folds the .spv bytes' hash into the preprocessed
  # source of every #embed-consuming TU (renderer.c / scene.c / effect.c
  # include it unconditionally), so the cache key tracks shader content.
  flux_extra_inputs += custom_target('flux_shader_manifest.h',
    input   : flux_shader_spvs,
    output  : 'flux_shader_manifest.h',
    command : [spv2h_py, files('../../tools/shader_manifest.py'), '@OUTPUT@', '@INPUT@'],
  )
endif

# Text shaping lives in the flux-text sibling (ADR-0016); libflux itself
# depends on Vulkan + threads only. The glyph-blit primitive
# flux_canvas_draw_glyph_run remains in the canvas module — flux-text feeds it.

# SO versioning (libtool semantics, per docs/reference/api.md):
#   - `soversion` (the SONAME) encodes the *compatibility boundary*. The
#     policy table says pre-1.0 patch releases are binary compatible and
#     minor releases may break; therefore the SONAME is "major.minor"
#     (optics_soname, derived once in the top-level meson.build).
#   - `version` (the real-name suffix) is the full X.Y.Z so each release
#     is a distinct file on disk while ld.so still resolves to the same
#     SONAME across patches.
# Carrying the full X.Y.Z in the SONAME (as this file once did) changes
# the link name on every patch release and contradicts the policy table.

flux_lib = library('flux',
  flux_sources,
  flux_extra_inputs,
  include_directories   : flux_inc,
  c_args                : flux_c_args,
  dependencies          : [vulkan_dep, threads_dep, m_dep],
  gnu_symbol_visibility : 'hidden',
  soversion             : optics_soname,
  version               : meson.project_version(),
  install               : true,
)

install_headers(
  'include/flux/flux.h',
  'include/flux/core.h',
  'include/flux/math.h',
  'include/flux/vulkan.h',
  'include/flux/dmabuf.h',
  subdir : 'flux',
)
if opt_canvas
  install_headers('include/flux/canvas.h', subdir : 'flux')
  install_headers('include/flux/canvas_cpu.h', subdir : 'flux')
endif
if opt_scene
  install_headers('include/flux/scene.h', subdir : 'flux')
endif
if opt_compute
  install_headers('include/flux/compute.h', subdir : 'flux')
endif
if opt_effect
  install_headers('include/flux/effect.h', subdir : 'flux')
endif
# Text header lives in the flux-text sibling (ADR-0016); not installed here.

# Public defines propagated to every consumer so the umbrella header
# (<flux/flux.h>) knows which optional module headers to pull in.
flux_public_defines = []
if have_dmabuf
  flux_public_defines += '-DFLUX_HAVE_DMABUF=1'
endif
if opt_canvas
  flux_public_defines += '-DFLUX_HAVE_CANVAS=1'
endif
if opt_scene
  flux_public_defines += '-DFLUX_HAVE_SCENE=1'
endif
if opt_compute
  flux_public_defines += '-DFLUX_HAVE_COMPUTE=1'
endif
if opt_effect
  flux_public_defines += '-DFLUX_HAVE_EFFECT=1'
endif

flux_dep = declare_dependency(
  link_with             : flux_lib,
  include_directories   : flux_inc,
  compile_args          : flux_public_defines,
  dependencies          : [vulkan_dep],
)

pkg.generate(
  libraries   : flux_lib,
  name        : 'flux',
  filebase    : 'flux',
  description  : 'C23 Vulkan-first graphics library (canvas, scene, compute, effect)',
  version      : meson.project_version(),
  extra_cflags : flux_public_defines,
)

glfw_dep = dependency('glfw3', required : false)

# Sibling content libraries (ADR-0016): they link libflux and feed its
# draw primitives. Each is a separate shared library with its own .pc.
if get_option('text')
  subdir('text')
else
  # Always defined so tests/examples can reference it unconditionally; empty
  # when the sibling is off (and FLUX_TEXT_HAVE_FTHB is then absent, so the
  # integration test's text path compiles out).
  flux_text_dep = declare_dependency()
endif
if get_option('scene-graph')
  subdir('scene_graph')
endif
