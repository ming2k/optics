# tests/prism — material library suites. The integration suite composites
# prism materials over flux canvas-target captures, so it additionally
# needs the flux effect module (the reusable blur filter) enabled.
if get_option('prism')
  subdir('unit')
  if get_option('effect')
    subdir('integration')
  endif
endif
