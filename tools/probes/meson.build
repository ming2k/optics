# tools/probes — developer pacing probes (ADR-0171). They are not examples:
# each prints frame-loop counters to stderr for a human to read against
# `top` / `time -v`. Built with the examples so they keep compiling against
# the current API.

if opt_ui
  foreach probe : ['frame_skip_demo', 'paint_static_demo']
    executable(probe,
      probe + '.c',
      dependencies : [app_dep, window_dep, ui_dep, gfx_dep],
      install      : false,
    )
  endforeach
endif
