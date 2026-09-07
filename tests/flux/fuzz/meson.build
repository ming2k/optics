# tests/flux/fuzz — fuzz harnesses
#
# Each harness exposes LLVMFuzzerTestOneInput and can be linked two ways:
#
#   * Standalone (default, run from meson test): the harness provides its
#     own main() that reads a single test case from argv[1] (or stdin) and
#     calls LLVMFuzzerTestOneInput once. Use this to repro a crash found by
#     a real fuzzer.
#
#   * libFuzzer (probed at the root meson.build; requires the clang
#     runtime): libFuzzer supplies main(); the harness is linked with
#     -fsanitize=fuzzer,address. Run the resulting binary directly — see
#     .github/workflows/ci-fuzz.yml for the invocation the CI uses.
#
# Seed corpus: corpus/<harness>/*.bin is copied into the build tree and
# each seed is replayed through the standalone driver as its own meson
# test (suite 'fuzz'), so a regression in one seed names the seed. The
# same directory is what the libFuzzer CI job passes as the corpus — one
# set of files, two consumers.

fs = import('fs')

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
  'corpus': 'corpus/path_flatten',
}

  # GLB parser: feed arbitrary bytes through the deviceless parse seam
  # (flux_sg_parse_glb, ADR-0016). Public API; libfuzzer-built via the
  # ci-fuzz workflow (see .github/workflows/ci-fuzz.yml matrix).
  fuzz_harnesses += {
    'name': 'fuzz_glb_parse',
    'src': files('fuzz_glb_parse.c'),
    'deps': [flux_sg_dep, m_dep],
    'corpus': 'corpus/glb_parse',
  }

if get_option('text')
  # Text shaping: feed arbitrary / truncated UTF-8 through the measure-only
  # seam (no device needed): measure, caret mapping, bidi cursor moves, and
  # selection rects at every truncation boundary. Asserts finite metrics,
  # ordered selection rects, and in-range cursor state; ASan catches the rest.
  fuzz_harnesses += {
    'name': 'fuzz_text_shape',
    'src': files('fuzz_text_shape.c'),
    'deps': [flux_text_dep, flux_dep, m_dep],
    'corpus': 'corpus/text_shape',
  }
endif

foreach h : fuzz_harnesses
  srcs = h['src']
  deps = h['deps']
  if libfuzzer_enabled
    exe = executable(h['name'], srcs,
      include_directories : flux_fuzz_inc,
      dependencies        : deps,
      c_args              : libfuzzer_c_args,
      link_args           : libfuzzer_link_args,
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

  # Deploy the seed corpus into the build tree and replay every seed
  # through the standalone driver (libFuzzer builds replay the corpus
  # from the CI job instead, so the per-seed tests are standalone-only).
  corpus_dir = h.get('corpus', '')
  if not libfuzzer_enabled and corpus_dir != ''
    seeds = []
    foreach f : run_command('find', meson.current_source_dir() / corpus_dir,
                            '-name', '*.bin', check : true).stdout().split('\n')
      if f != ''
        seeds += configure_file(input : f,
          output : fs.name(f),
          copy : true)
      endif
    endforeach
    foreach s : seeds
      test(h['name'] + '_' + fs.name(s), exe,
        args : [s],
        suite : 'fuzz',
        should_fail : false,
      )
    endforeach
  endif
endforeach
