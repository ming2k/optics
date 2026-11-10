# Tooling self-tests. A guardrail that has never been observed failing is
# indistinguishable from one that cannot fail, so every guardrail with a
# non-trivial decision table carries a negative test here (ADR-0130).

py = import('python').find_installation('python3')

# The leak guardrail reads ELF SONAMEs and libflux.so; keep the self-test on
# the platform where that mechanism exists.
if host_machine.system() == 'linux'
  test(
    'leaks_guardrail',
    py,
    args : [files('test_leaks_guardrail.py')],
    workdir : meson.project_source_root(),
    suite : 'tooling',
    timeout : 120,
  )
endif

test(
  'listeners_guardrail',
  py,
  args : [files('test_listeners_guardrail.py')],
  workdir : meson.project_source_root(),
  suite : 'tooling',
  timeout : 120,
)
