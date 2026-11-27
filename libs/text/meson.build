# liboptics_text — HarfBuzz shaping and dynamic glyph atlas runtime (ADR-0016, ADR-0106, ADR-0170).
#
# A separate shared library that links liboptics_gfx and feeds the
# optics_gfx_canvas_draw_glyph_run primitive. It owns FreeType + HarfBuzz +
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
  dxguid_dep = text_cc.find_library('dxguid', required : true)
  text_backend_deps += [dwrite_dep, uuid_dep, dxguid_dep]
  text_platform_sources = files('src/txt_platform_font_directwrite.c')
elif text_host_system == 'darwin'
  apple_dep = dependency('appleframeworks',
                         modules : ['CoreText', 'CoreFoundation'],
                         required : true)
  text_backend_deps += apple_dep
  text_platform_sources = files('src/txt_platform_font_coretext.c')
else
  error('optics-text: no font-discovery backend for host system "@0@"'.format(text_host_system))
endif

text_inc = include_directories('include')

text_sources = files(
  'src/version.c',
  'src/mono_stub.c',
  'src/face.c',
  'src/glyph_cache.c',
  'src/atlas.c',
  'src/itemize.c',
  'src/layout.c',
) + text_platform_sources

text_c_args = ['-DOPTICS_TEXT_BUILDING=1', '-DOPTICS_TEXT_HAVE_FTHB=1']

text_lib = library('optics_text',
  text_sources,
  include_directories : text_inc,
  c_args              : text_c_args,
  dependencies        : [gfx_dep, m_dep] + text_backend_deps,
  gnu_symbol_visibility : 'hidden',
  soversion           : optics_soname,
  version             : meson.project_version(),
  install             : true,
)

install_headers('include/optics/text.h', subdir : 'optics')

text_public_defines = ['-DOPTICS_TEXT_HAVE_FTHB=1']

text_dep = declare_dependency(
  link_with           : text_lib,
  include_directories : text_inc,
  compile_args        : text_public_defines,
  dependencies        : [gfx_dep],
)

pkg.generate(
  libraries    : text_lib,
  name         : 'optics-text',
  filebase     : 'optics-text',
  description  : 'HarfBuzz shaping and dynamic glyph atlas runtime over gfx',
  version      : meson.project_version(),
  requires     : ['optics-gfx >= ' + meson.project_version()],
  extra_cflags : text_public_defines,
)
