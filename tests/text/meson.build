# glyph — shaping, layout, caret/selection mapping and atlas suites.
#
# The pure cache TU is unit-testable on any host. Anything that shapes text
# crosses the font stack boundary and therefore lives in integration, where a
# missing font/backend is reported as a skip rather than a vacuous pass.

if get_option('text')
  text_test_dep = declare_dependency(
    dependencies : [text_dep, gfx_dep, m_dep, optics_test_dep],
  )

  subdir('unit')
  subdir('integration')
endif
