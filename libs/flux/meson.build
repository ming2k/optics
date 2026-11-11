#Single first - party library : libflux.The C source tree sits at the repo
#root(`src /`, `include /`, `tests /`, `examples /`).Content layers that feed
#flux's draw primitives live in `libs/` as siblings (ADR-0016, ADR-0106): `glyph`
#(HarfBuzz shaping, feeds flux_canvas_draw_glyph_run) and `vista`
#(glTF, feeds flux_scene_draw_mesh).Rust bindings live under `bindings /`.

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
if get_option('glyph') and not opt_canvas
  error('glyph sibling requires canvas (flux_canvas_draw_glyph_run) — pass -Dcanvas=true with -Dglyph=true')
endif

flux_inc = [include_directories('include'), include_directories('.')]

#dma - buf(fd - based external memory import / export + sync_file fences) is
#Linux - only.When off, src / core / dmabuf_stub.c replaces dmabuf.c and
#answers every public entry point FLUX_ERROR_UNSUPPORTED.The sources are
#core - level, NOT part of the canvas module gate : device.c / surface.c /
#frame.c call the acquire - semaphore pool helpers and flux_dmabuf_supported
#unconditionally(ADR - 0052 : no #ifdef at call sites), so a - Dcanvas = false
#build must still link them.The one canvas - internal entry point lives in
#src / canvas / dmabuf_acquire.c inside the canvas gate.
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
  'src/canvas/image.c',
  'src/math/vec.c',
  'src/math/mat.c',
  'src/math/quat.c',
  'src/math/color.c',
  'src/math/colorspace.c',
  'src/math/arena.c',
  'src/curve/curve.c',
  'src/sdf/sdf.c',
  'src/vault/tess_earcut.c',
)
if have_dmabuf
  flux_sources += files('src/core/dmabuf.c')
else
  flux_sources += files('src/core/dmabuf_stub.c')
endif
flux_extra_inputs = []

flux_c_args = ['-DFLUX_BUILDING=1'] + flux_c_args_dma

#Observation - plane attribution(ADR - 0129).Off by default so a release
#build carries no name / site machinery; ON is what makes FLUX_OBSERVE_NAMES,
#FLUX_OBSERVE_PASS_LABELS and FLUX_OBSERVE_SITES cost anything.The absence
#of the symbols in a default build is verified by./ tools / optics check
#leaks, so "debug only" is a checked property rather than a promise.
opt_observe_attribution = get_option('observe_attribution')
if opt_observe_attribution
  flux_c_args += '-DFLUX_OBSERVE_ATTRIBUTION=1'
endif

#C23 #embed bakes each compiled.spv into the binary(renderer.c /
#effect.c / scene.c).Compilers without #embed(MSVC cl, Apple Clang)
#take a generated - header path instead — the headers are produced from
#the same.spv files by tools / build / spv2h.py further below.--embed - dir is
#probed because MSVC rejects unknown options outright.
flux_embed_dir_arg = cc.get_supported_arguments('--embed-dir=' + meson.current_build_dir())
if opt_canvas or opt_scene or opt_effect
  flux_c_args += flux_embed_dir_arg
endif
#One Slang discovery for every shader - bearing module.Defining it here(not
#inside the canvas block) keeps `- Dcanvas = false - Dscene = true` and
# `- Dcanvas = false - Deffect = true` valid configurations : both compile shaders.
if opt_canvas or opt_scene or opt_effect
  slangc = find_program('slangc', required : true)
  slangc_flags = ['-target', 'spirv', '-profile', 'spirv_1_6', '-Wno-41012']
endif

#Per - shader accumulators for the generated - header fallback; the
#custom_target loop sits after the module blocks.
flux_shader_spvs = []
flux_shader_names = []
if opt_canvas
  flux_sources += files(
    'src/canvas/canvas.c',
    'src/canvas/canvas_dispatch.c',
    'src/canvas/path.c',
    'src/canvas/paint.c',
    'src/canvas/geometry_tess.c',
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
  )
  flux_c_args += '-DFLUX_HAVE_CANVAS=1'

  canvas_shader_deps = [
    files('src/canvas/shaders/canvas_color.slang'),
    files('include/flux/shaders/flux_sdf.slang'),
    files('include/flux/shaders/flux_vector_eval.slang'),
  ]
  canvas_shader_inc = [
    '-I' + meson.current_source_dir() / 'src/canvas/shaders',
    '-I' + meson.current_source_dir() / 'include',
    '-I' + meson.current_source_dir() / 'include/flux/shaders',
  ]
  canvas_solid_vert_spv = custom_target('canvas_solid.vert.spv',
    input   : 'src/canvas/shaders/canvas_solid.vert.slang',
    depend_files : canvas_shader_deps,
    output  : 'canvas_solid.vert.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'vertex',
               canvas_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  canvas_solid_frag_spv = custom_target('canvas_solid.frag.spv',
    input   : 'src/canvas/shaders/canvas_solid.frag.slang',
    depend_files : canvas_shader_deps,
    output  : 'canvas_solid.frag.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'fragment',
               canvas_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  canvas_gradient_frag_spv = custom_target('canvas_gradient.frag.spv',
    input   : 'src/canvas/shaders/canvas_gradient.frag.slang',
    depend_files : canvas_shader_deps,
    output  : 'canvas_gradient.frag.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'fragment',
               canvas_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  canvas_image_frag_spv = custom_target('canvas_image.frag.spv',
    input   : 'src/canvas/shaders/canvas_image.frag.slang',
    depend_files : canvas_shader_deps,
    output  : 'canvas_image.frag.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'fragment',
               canvas_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  canvas_glyph_frag_spv = custom_target('canvas_glyph.frag.spv',
    input   : 'src/canvas/shaders/canvas_glyph.frag.slang',
    depend_files : canvas_shader_deps,
    output  : 'canvas_glyph.frag.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'fragment',
               canvas_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  canvas_sdf_frag_spv = custom_target('canvas_sdf.frag.spv',
    input   : 'src/canvas/shaders/canvas_sdf.frag.slang',
    depend_files : canvas_shader_deps,
    output  : 'canvas_sdf.frag.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'fragment',
               canvas_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  canvas_output_vert_spv = custom_target('canvas_output.vert.spv',
    input   : 'src/canvas/shaders/canvas_output.vert.slang',
    depend_files : canvas_shader_deps,
    output  : 'canvas_output.vert.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'vertex',
               canvas_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  canvas_output_frag_spv = custom_target('canvas_output.frag.spv',
    input   : 'src/canvas/shaders/canvas_output.frag.slang',
    depend_files : canvas_shader_deps,
    output  : 'canvas_output.frag.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'fragment',
               canvas_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  canvas_tile_raster_comp_spv = custom_target('canvas_tile_raster.comp.spv',
    input   : 'src/canvas/shaders/canvas_tile_raster.comp.slang',
    output  : 'canvas_tile_raster.comp.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
               canvas_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  canvas_vector_frag_spv = custom_target('canvas_vector.frag.spv',
    input   : 'src/canvas/shaders/canvas_vector.frag.slang',
    depend_files : canvas_shader_deps,
    output  : 'canvas_vector.frag.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'fragment',
               canvas_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  flux_extra_inputs += [canvas_solid_vert_spv, canvas_solid_frag_spv,
                        canvas_gradient_frag_spv, canvas_image_frag_spv,
                        canvas_glyph_frag_spv, canvas_sdf_frag_spv,
                        canvas_output_vert_spv, canvas_output_frag_spv,
                        canvas_tile_raster_comp_spv, canvas_vector_frag_spv]
  flux_shader_spvs += [canvas_solid_vert_spv, canvas_solid_frag_spv,
                       canvas_gradient_frag_spv, canvas_image_frag_spv,
                       canvas_glyph_frag_spv, canvas_sdf_frag_spv,
                       canvas_output_vert_spv, canvas_output_frag_spv,
                       canvas_tile_raster_comp_spv, canvas_vector_frag_spv]
  flux_shader_names += ['canvas_solid_vert_spv', 'canvas_solid_frag_spv',
                        'canvas_gradient_frag_spv', 'canvas_image_frag_spv',
                        'canvas_glyph_frag_spv', 'canvas_sdf_frag_spv',
                        'canvas_output_vert_spv', 'canvas_output_frag_spv',
                        'canvas_tile_raster_comp_spv', 'canvas_vector_frag_spv']
endif
if opt_scene
  flux_sources += files('src/scene/scene.c')
  flux_c_args += '-DFLUX_HAVE_SCENE=1'

  scene_shader_deps = [
    files('src/scene/shaders/scene_color.slang'),
    files('src/canvas/shaders/canvas_color.slang'),
    files('include/flux/shaders/flux_vector_eval.slang'),
  ]
  scene_shader_inc = [
    '-I' + meson.current_source_dir() / 'src/canvas/shaders',
    '-I' + meson.current_source_dir() / 'src/scene/shaders',
    '-I' + meson.current_source_dir() / 'include',
    '-I' + meson.current_source_dir() / 'include/flux/shaders',
  ]
  scene_unlit_vert_spv = custom_target('scene_unlit.vert.spv',
    input   : 'src/scene/shaders/scene_unlit.vert.slang',
    depend_files : scene_shader_deps,
    output  : 'scene_unlit.vert.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'vertex',
               scene_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  scene_unlit_frag_spv = custom_target('scene_unlit.frag.spv',
    input   : 'src/scene/shaders/scene_unlit.frag.slang',
    depend_files : scene_shader_deps,
    output  : 'scene_unlit.frag.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'fragment',
               scene_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  scene_phong_vert_spv = custom_target('scene_phong.vert.spv',
    input   : 'src/scene/shaders/scene_phong.vert.slang',
    depend_files : scene_shader_deps,
    output  : 'scene_phong.vert.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'vertex',
               scene_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  scene_phong_frag_spv = custom_target('scene_phong.frag.spv',
    input   : 'src/scene/shaders/scene_phong.frag.slang',
    depend_files : scene_shader_deps,
    output  : 'scene_phong.frag.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'fragment',
               scene_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  scene_unlit_skin_vert_spv = custom_target('scene_unlit_skin.vert.spv',
    input   : 'src/scene/shaders/scene_unlit_skin.vert.slang',
    depend_files : scene_shader_deps,
    output  : 'scene_unlit_skin.vert.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'vertex',
               scene_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  scene_phong_skin_vert_spv = custom_target('scene_phong_skin.vert.spv',
    input   : 'src/scene/shaders/scene_phong_skin.vert.slang',
    depend_files : scene_shader_deps,
    output  : 'scene_phong_skin.vert.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'vertex',
               scene_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  scene_vector_vert_spv = custom_target('scene_vector.vert.spv',
    input   : 'src/scene/shaders/scene_vector.vert.slang',
    depend_files : scene_shader_deps,
    output  : 'scene_vector.vert.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'vertex',
               scene_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  scene_vector_frag_spv = custom_target('scene_vector.frag.spv',
    input   : 'src/scene/shaders/scene_vector.frag.slang',
    depend_files : scene_shader_deps,
    output  : 'scene_vector.frag.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'fragment',
               scene_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  flux_extra_inputs += [scene_unlit_vert_spv, scene_unlit_frag_spv,
                        scene_phong_vert_spv, scene_phong_frag_spv,
                        scene_unlit_skin_vert_spv, scene_phong_skin_vert_spv,
                        scene_vector_vert_spv, scene_vector_frag_spv]
  flux_shader_spvs += [scene_unlit_vert_spv, scene_unlit_frag_spv,
                       scene_phong_vert_spv, scene_phong_frag_spv,
                       scene_unlit_skin_vert_spv, scene_phong_skin_vert_spv,
                       scene_vector_vert_spv, scene_vector_frag_spv]
  flux_shader_names += ['scene_unlit_vert_spv', 'scene_unlit_frag_spv',
                        'scene_phong_vert_spv', 'scene_phong_frag_spv',
                        'scene_unlit_skin_vert_spv', 'scene_phong_skin_vert_spv',
                        'scene_vector_vert_spv', 'scene_vector_frag_spv']
endif
if opt_compute
  flux_sources += files('src/compute/compute.c')
  flux_c_args += '-DFLUX_HAVE_COMPUTE=1'
endif
if opt_effect
  flux_sources += files('src/effect/effect.c')
  flux_c_args += '-DFLUX_HAVE_EFFECT=1'

  effect_shader_inc = [
    '-I' + meson.current_source_dir() / 'src/canvas/shaders',
    '-I' + meson.current_source_dir() / 'include',
    '-I' + meson.current_source_dir() / 'include/flux/shaders',
  ]
  effect_blur_comp_spv = custom_target('effect_blur.comp.spv',
    input   : 'src/effect/shaders/effect_blur.comp.slang',
    output  : 'effect_blur.comp.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
               effect_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  effect_backdrop_comp_spv = custom_target('effect_backdrop.comp.spv',
    input   : 'src/effect/shaders/effect_backdrop.comp.slang',
    output  : 'effect_backdrop.comp.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
               effect_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
#ADR - 0069 : rgba16f storage variants for working - space(16F) content.
  effect_blur_16f_comp_spv = custom_target('effect_blur_16f.comp.spv',
    input   : 'src/effect/shaders/effect_blur.comp.slang',
    output  : 'effect_blur_16f.comp.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
               '-DEFFECT_STORAGE_RGBA16F', effect_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  effect_backdrop_16f_comp_spv = custom_target('effect_backdrop_16f.comp.spv',
    input   : 'src/effect/shaders/effect_backdrop.comp.slang',
    output  : 'effect_backdrop_16f.comp.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
               '-DEFFECT_STORAGE_RGBA16F', effect_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
#ADR - 0074 : drop - shadow operator(mask in, tinted premultiplied shadow out).
  effect_shadow_comp_spv = custom_target('effect_shadow.comp.spv',
    input   : 'src/effect/shaders/effect_shadow.comp.slang',
    output  : 'effect_shadow.comp.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
               effect_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  effect_shadow_16f_comp_spv = custom_target('effect_shadow_16f.comp.spv',
    input   : 'src/effect/shaders/effect_shadow.comp.slang',
    output  : 'effect_shadow_16f.comp.spv',
    command : [slangc, slangc_flags, '-entry', 'main', '-stage', 'compute',
               '-DEFFECT_STORAGE_RGBA16F', effect_shader_inc, '@INPUT@', '-o', '@OUTPUT@'],
  )
  flux_extra_inputs += [effect_blur_comp_spv, effect_backdrop_comp_spv,
                        effect_blur_16f_comp_spv, effect_backdrop_16f_comp_spv,
                        effect_shadow_comp_spv, effect_shadow_16f_comp_spv]
  flux_shader_spvs += [effect_blur_comp_spv, effect_backdrop_comp_spv,
                       effect_blur_16f_comp_spv, effect_backdrop_16f_comp_spv,
                       effect_shadow_comp_spv, effect_shadow_16f_comp_spv]
  flux_shader_names += ['effect_blur_spv', 'effect_backdrop_spv',
                        'effect_blur_16f_spv', 'effect_backdrop_16f_spv',
                        'effect_shadow_spv', 'effect_shadow_16f_spv']
endif

#Generated shader headers for the no - #embed fallback(MSVC cl / Apple
#Clang).The C sources pick per translation unit via __has_embed, so the
#headers must exist regardless of host compiler; each is one small
#script run over the matching.spv.
if flux_shader_spvs.length() > 0
  spv2h_py = import('python').find_installation('python3')
  spv2h = files('../../tools/build/spv2h.py')
  foreach i : range(flux_shader_spvs.length())
    flux_extra_inputs += custom_target(flux_shader_names[i] + '.h',
      input   : flux_shader_spvs[i],
      output  : flux_shader_names[i] + '.h',
      command : [spv2h_py, spv2h, '@INPUT@', '@OUTPUT@', flux_shader_names[i]],
    )
  endforeach
#ccache does not hash C23 #embed payloads(observed 4.13.6) : without
#this, editing a shader silently reuses the previous object.The
#manifest header folds the.spv bytes' hash into the preprocessed
#source of every #embed - consuming TU(renderer.c / scene.c / effect.c
#include it unconditionally), so the cache key tracks shader content.
  flux_extra_inputs += custom_target('flux_shader_manifest.h',
    input   : flux_shader_spvs,
    output  : 'flux_shader_manifest.h',
    command : [spv2h_py, files('../../tools/build/shader_manifest.py'), '@OUTPUT@', '@INPUT@'],
  )
endif

#Text shaping lives in the glyph sibling(ADR - 0016, ADR - 0106); libflux itself
#depends on Vulkan + threads only.The glyph - blit primitive
#flux_canvas_draw_glyph_run remains in the canvas module — glyph feeds it.

#SO versioning(libtool semantics, per docs / reference / api.md):
#- `soversion` (the SONAME) encodes the * compatibility boundary *.Pre - 1.0
#this tree has appended descriptor fields and changed pass behavior on
#patch bumps, so the boundary is the full version(`0.0.77`).From 1.0 on
#it is `major.minor`; `optics_soname` is derived once in the top - level
#meson.build.
#- `version` (the real - name suffix) is the full X.Y.Z so each release is a
#distinct file on disk.
#Keeping a binary linked to one pre - 1.0 release from silently loading another
#is the point : the SONAMEs differ and ld.so fails fast instead of reading a
#descriptor whose layout changed.

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
  subdir : 'flux',
)

#The analytic distance field is defined once for Slang consumers too
#(ADR - 0118, ADR - 0156); Prism's materials include it rather than keeping private copies.
install_headers('include/flux/shaders/flux_sdf.slang',
                'include/flux/shaders/flux_vector_eval.slang',
                subdir : 'flux/shaders')
install_headers('include/flux/vault/tess_earcut.h', subdir : 'flux/vault')
if opt_canvas
  install_headers('include/flux/canvas.h', subdir : 'flux')
  install_headers('include/flux/canvas_helpers.h', subdir : 'flux')
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
#Text header lives in the glyph sibling(ADR - 0016, ADR - 0106); not installed here.

#Public defines propagated to every consumer so the umbrella header
#(<flux / flux.h>) knows which optional module headers to pull in.
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
