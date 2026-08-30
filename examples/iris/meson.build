# Windowed C demos — all use the iris_app_run entry point.
#
# Each demo owns no window or event loop; iris_app_run does. The build
# callback runs once per frame inside an open lens_begin/end pair.

example_deps = [iris_dep, m_dep]

foreach demo : ['hello_app', 'minimal', 'widgets', 'fonts', 'overlay_demo', 'forms', 'desktop_demo', 'paint_static_demo', 'dnd_demo']
  executable(demo, demo + '.c',
    dependencies        : example_deps,
    include_directories : iris_inc,
  )
endforeach
