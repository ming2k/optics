# tests/flux/fuzz — fuzz harnesses
#
# Each harness exposes LLVMFuzzerTestOneInput and can be linked two ways:
#
#   * Standalone (default, run from meson test): the harness provides its
#     own main() that reads a single test case from argv[1] (or stdin) and
#     calls LLVMFuzzerTestOneInput once. Use this to repro a crash found by
#     a real fuzzer.
#
#   * libFuzzer (set LIBFUZZER=1 on the environment when configuring meson
#     with -Dfuzz=true): libFuzzer supplies main(); the harness is linked
#     with -fsanitize=fuzzer,address. Run the resulting binary directly.
#
# Harnesses are excluded from the default `meson test` run via the 'fuzz'
# suite — invoke explicitly with `meson test -C build --suite fuzz`.

flux_fuzz_inc = include_directories('..')

# Common harness shim providing a standalone main() that reads a file from
# argv[1] (or stdin) and forwards it to LLVMFuzzerTestOneInput. When built
# with libFuzzer this object is replaced by libFuzzer's own main.
fuzz_main_src = files('fuzz_main.c')

fuzz_harnesses = []

# Path flattener: feed the fuzzer random verb sequences. The path parser
# interprets uint8 op codes in {0..4} as move/line/quad/cubic/close; the
# rest of the bytes fill the verb arguments. This stresses the flatten
# tolerances, the contour allocator, and the stroke + ear-clip stack.
fuzz_harnesses += {
  'name': 'fuzz_path_flatten',
  'src': files('fuzz_path_flatten.c'),
  'deps': [flux_dep, m_dep],
}

if get_option('scene-graph')
  # GLB parser: feed random bytes as a fake glb file. The parser validates
  # the magic, chunk lengths, and JSON header; harness asserts it either
  # succeeds with a discovered mesh count or returns a non-OK flux_result.
  #
  # DISABLED for now: flux_sg_load_glb requires a flux_device*; fuzzing it
  # without a GPU requires a deviceless parser entry point we do not yet
  # expose. See ADR-0016 for the pure-RHI direction that would unlock this.
  # The harness source is kept so it can be re-enabled when the seam lands.
  fuzz_harnesses += {
    'name': 'fuzz_glb_parse_disabled',
    'src': files('fuzz_glb_parse_disabled.c'),
    'deps': [m_dep],
  }
endif

if get_option('text')
  # Text shaping: feed random UTF-8 to the itemizer. BiDi / script
  # detection runs over arbitrary bytes; the harness asserts no UB / leak.
  fuzz_harnesses += {
    'name': 'fuzz_text_shape',
    'src': files('fuzz_text_shape.c'),
    'deps': [flux_text_dep, flux_dep, m_dep],
  }
endif

foreach h : fuzz_harnesses
  srcs = h['src']
  deps = h['deps']
  # When LIBFUZZER is requested, do not pull in our standalone main().
  if get_option('fuzz') and get_variable('libfuzzer_enabled', false)
    exe = executable(h['name'], srcs,
      include_directories : flux_fuzz_inc,
      dependencies        : deps,
      c_args              : ['-fsanitize=fuzzer-no-link'],
      link_args           : ['-fsanitize=fuzzer'],
      install             : false,
    )
  else
    exe = executable(h['name'], srcs + fuzz_main_src,
      include_directories : flux_fuzz_inc,
      dependencies        : deps,
      install             : false,
    )
    test(h['name'], exe, suite : 'fuzz', should_fail : false)
  endif
endforeach
