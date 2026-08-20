# Benchmarks: CPU-side performance measurements. Registered under the
# 'bench' suite so `meson test --suite bench` runs only these.
#
# Each benchmark is a standalone executable that prints timing results
# to stdout. They return 0 unconditionally (no pass/fail); CI should
# parse the output if regression gating is desired.

bench_helpers_inc = include_directories('.')

bench_math = executable('bench_math',
  'bench_math.c',
  include_directories : bench_helpers_inc,
  dependencies : [test_helpers_dep],
)
test('math', bench_math, suite : 'bench', is_parallel : false)

bench_arena = executable('bench_arena',
  'bench_arena.c',
  include_directories : bench_helpers_inc,
  dependencies : [test_helpers_dep],
)
test('arena', bench_arena, suite : 'bench', is_parallel : false)

if opt_canvas
  bench_flatten = executable('bench_flatten',
    'bench_flatten.c',
    '../../../libs/flux/src/canvas/geometry_flatten.c',
    include_directories : bench_helpers_inc,
    dependencies : [test_helpers_dep],
  )
  test('flatten', bench_flatten, suite : 'bench', is_parallel : false)

  bench_tess = executable('bench_tess',
    'bench_tess.c',
    '../../../libs/flux/src/canvas/geometry_tess.c',
    include_directories : bench_helpers_inc,
    dependencies : [test_helpers_dep],
  )
  test('tess', bench_tess, suite : 'bench', is_parallel : false)

  bench_path = executable('bench_path',
    'bench_path.c',
    include_directories : bench_helpers_inc,
    dependencies : [test_helpers_dep],
  )
  test('path', bench_path, suite : 'bench', is_parallel : false)

  # GPU compositor-shaped frame loop: one surface pass + two render-target
  # captures per frame, timed end to end.
  bench_frame_loop = executable('bench_frame_loop',
    'bench_frame_loop.c',
    include_directories : bench_helpers_inc,
    dependencies : [test_helpers_dep],
  )
  test('frame_loop', bench_frame_loop, suite : 'bench', is_parallel : false)
endif
