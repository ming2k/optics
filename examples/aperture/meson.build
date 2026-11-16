# Windowed lens demos — all use the aperture_app_run entry point.
#
# Each demo owns no window or event loop; iris does, through aperture. The build
# callback runs once per frame inside an open lens_begin/end pair.

example_deps = [aperture_dep, m_dep]

foreach demo : ['hello_window', 'widgets', 'fonts', 'overlay_demo', 'desktop_demo', 'paint_static_demo', 'frame_skip_demo', 'dnd_demo']
  executable(demo, demo + '.c',
    dependencies        : example_deps,
    include_directories : iris_inc,
  )
endforeach
