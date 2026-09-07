# tests/lens/fuzz — lens-side fuzz harnesses.
#
# Same build wiring as tests/flux/fuzz/meson.build: a standalone main()
# is pulled in from fuzz_main.c, or replaced by libFuzzer's main() when
# the runtime was probed at the root meson.build (libfuzzer_enabled).
# The seed corpus under corpus/<harness>/ is deployed into the build
# tree and each seed is replayed as its own meson test (suite 'fuzz');
# the libFuzzer CI job (.github/workflows/ci-fuzz.yml) feeds the same
# directory as the corpus argument.

fs = import('fs')

lens_fuzz_inc = include_directories('..')
fuzz_main_src = files('../../flux/fuzz/fuzz_main.c')

lens_fuzz_harnesses = [
  {
    'name': 'fuzz_lens_store',
    'src': files('fuzz_lens_store.c'),
    'deps': [lens_dep],
    'corpus': 'corpus/lens_store',
  },
]

foreach h : lens_fuzz_harnesses
  if libfuzzer_enabled
    exe = executable(h['name'], h['src'],
      include_directories : lens_fuzz_inc,
      dependencies        : h['deps'],
      c_args              : libfuzzer_c_args,
      link_args           : libfuzzer_link_args,
      install             : false,
    )
  else
    exe = executable(h['name'], h['src'] + fuzz_main_src,
      include_directories : lens_fuzz_inc,
      dependencies        : h['deps'],
      install             : false,
    )
    test(h['name'], exe, suite : 'fuzz', should_fail : false)
  endif

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
