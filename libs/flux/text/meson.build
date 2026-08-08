# flux-text — HarfBuzz shaping sibling over libflux (ADR-0016).
#
# A separate shared library that links libflux and feeds the
# flux_canvas_draw_glyph_run primitive. It owns FreeType + HarfBuzz +
# Fontconfig + FriBidi, the glyph atlas, BiDi itemisation, and caret/selection
# mapping. The implementation is the former libflux `text` module, relocated
# unchanged; only its library boundary moved.
#
# The shaping backend is mandatory here (the sibling's purpose is HarfBuzz
# shaping). Consumers that do not want it pass -Dtext=false to skip the whole
# sibling.

# FreeType (rasterisation), HarfBuzz (shaping) and FriBidi (BiDi) are
# platform-independent and required everywhere. Font DISCOVERY is the only
# platform-specific piece and lives behind src/txt_platform_font.h:
#   linux   → fontconfig  (src/txt_platform_font_fontconfig.c)
#   windows → DirectWrite (src/txt_platform_font_directwrite.c)
#   darwin  → CoreText    (src/txt_platform_font_coretext.c)
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
  # IID_* GUID symbols (IID_IDWriteFactory, IID_IDWriteFactory2, ...) live
  # in uuid.lib, which MSVC does not link by default outside IDE projects.
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
  error('flux-text: no font-discovery backend for host system "@0@"'.format(text_host_system))
endif

flux_text_inc = include_directories('include')

flux_text_sources = files(
  'src/version.c',
  'src/mono_stub.c',
  'src/face.c',
  'src/glyph_cache.c',
  'src/atlas.c',
  'src/itemize.c',
  'src/layout.c',
) + text_platform_sources

# FLUX_TEXT_HAVE_FTHB selects the real backend in text_internal.h. Propagated
# to consumers via flux_text_dep + the .pc Cflags so a user of <flux-text/text.h>
# sees the full API, not the monospace fallback.
flux_text_c_args = ['-DFLUX_TEXT_BUILDING=1', '-DFLUX_TEXT_HAVE_FTHB=1']

flux_text_lib = library('flux_text',
  flux_text_sources,
  include_directories : flux_text_inc,
  c_args              : flux_text_c_args,
  dependencies        : [flux_dep, m_dep] + text_backend_deps,
  gnu_symbol_visibility : 'hidden',
  install             : true,
)

install_headers('include/flux-text/text.h', subdir : 'flux-text')

flux_text_public_defines = ['-DFLUX_TEXT_HAVE_FTHB=1']

flux_text_dep = declare_dependency(
  link_with           : flux_text_lib,
  include_directories : flux_text_inc,
  compile_args        : flux_text_public_defines,
  dependencies        : [flux_dep],
)

pkg.generate(
  libraries    : flux_text_lib,
  name         : 'flux-text',
  filebase     : 'flux-text',
  description  : 'HarfBuzz shaping layer over flux (feeds flux_canvas_draw_glyph_run)',
  version      : meson.project_version(),
  requires     : 'flux',
  extra_cflags : flux_text_public_defines,
)


