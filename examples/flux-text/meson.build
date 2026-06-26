# flux-text examples. The hello demo is windowed (GLFW); skip cleanly when
# GLFW is absent, matching the flux/examples convention.
if not glfw_dep.found()
  warning('GLFW not found; skipping text_hello example.')
  subdir_done()
endif

executable('text_hello',
  'text_hello.c',
  dependencies : [flux_text_dep, glfw_dep, m_dep],
)
