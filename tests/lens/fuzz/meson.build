# tests/lens/fuzz — lens-side fuzz harnesses.
#
# Same build wiring as tests/flux/fuzz/meson.build: a standalone main()
# is pulled in from fuzz_main.c, or replaced by libFuzzer's main() when
# LIBFUZZER=1 is set at configure time + -Dfuzz=true.

lens_fuzz_inc = include_directories('..')
fuzz_main_src = files('../../flux/fuzz/fuzz_main.c')

lens_fuzz_harnesses = [
  {
    'name': 'fuzz_lens_store',
    'src': files('fuzz_lens_store.c'),
    'deps': [lens_dep],
  },
]

foreach h : lens_fuzz_harnesses
  exe = executable(h['name'], h['src'] + fuzz_main_src,
    include_directories : lens_fuzz_inc,
    dependencies        : h['deps'],
    install             : false,
  )
  test(h['name'], exe, suite : 'fuzz', should_fail : false)
endforeach
