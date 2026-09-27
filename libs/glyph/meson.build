# glyph — HarfBuzz shaping and dynamic glyph atlas runtime (ADR-0016, ADR-0106).
#
# A separate shared library that links libflux and feeds the
# flux_canvas_draw_glyph_run primitive. It owns FreeType + HarfBuzz +
# Fontconfig + FriBidi, the glyph atlas, BiDi itemisation, and caret/selection
# mapping.

ft_dep = dependency('freetype2',  required : true)
hb_dep = dependency('harfbuzz',   required : true)
fb_dep = dependency('fribidi',    required : true)
text_backend_deps = [ft_dep, hb_dep, fb_dep]

text_host_system = host_machine.system()
if text_host_system == 'linux'
  fc_dep = dependency('fontconfig', required : true)
  text_backend_deps += fc_dep
  text_platform_sources = files('src/txt_platform_font_fontconfig.c')
elif text_host_system == 'windows'
  text_cc = meson.get_compiler('c')
  dwrite_dep = text_cc.find_library('dwrite', required : true)
  uuid_dep = text_cc.find_library('uuid', required : true)
  text_backend_deps += [dwrite_dep, uuid_dep]
  text_platform_sources = files('src/txt_platform_font_directwrite.c')
elif text_host_system == 'darwin'
  apple_dep = dependency('appleframeworks',
                         modules : ['CoreText', 'CoreFoundation'],
                         required : true)
  text_backend_deps += apple_dep
  text_platform_sources = files('src/txt_platform_font_coretext.c')
else
  error('glyph: no font-discovery backend for host system "@0@"'.format(text_host_system))
endif

glyph_inc = include_directories('include')

glyph_sources = files(
  'src/version.c',
  'src/mono_stub.c',
  'src/face.c',
  'src/glyph_cache.c',
  'src/atlas.c',
  'src/itemize.c',
  'src/layout.c',
) + text_platform_sources

glyph_c_args = ['-DGLYPH_BUILDING=1', '-DGLYPH_HAVE_FTHB=1']

glyph_lib = library('glyph',
  glyph_sources,
  include_directories : glyph_inc,
  c_args              : glyph_c_args,
  dependencies        : [flux_dep, m_dep] + text_backend_deps,
  gnu_symbol_visibility : 'hidden',
  soversion           : optics_soname,
  version             : meson.project_version(),
  install             : true,
)

install_headers('include/glyph/glyph.h', subdir : 'glyph')

glyph_public_defines = ['-DGLYPH_HAVE_FTHB=1']

glyph_dep = declare_dependency(
  link_with           : glyph_lib,
  include_directories : glyph_inc,
  compile_args        : glyph_public_defines,
  dependencies        : [flux_dep],
)

pkg.generate(
  libraries    : glyph_lib,
  name         : 'glyph',
  filebase     : 'glyph',
  description  : 'HarfBuzz shaping and dynamic glyph atlas runtime over flux',
  version      : meson.project_version(),
  requires     : 'flux',
  extra_cflags : glyph_public_defines,
)
