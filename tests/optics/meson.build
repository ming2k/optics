# Code quality and tooling verification
py = import('python').find_installation('python3')

# Verify Wayland listener slot contract (ADR-0145) across the codebase
test(
  'wayland_listeners',
  py,
  args : [files('../../tools/check_listeners.py')],
  workdir : meson.project_source_root(),
  suite : ['lint', 'tooling'],
  timeout : 30,
)

# Negative regression test suite for the Wayland listener contract scanner
test(
  'listeners_guardrail_selftest',
  py,
  args : [files('test_listeners_guardrail.py')],
  workdir : meson.project_source_root(),
  suite : 'tooling',
  timeout : 30,
)

# Umbrella Facade and Integrated Dependencies (ADR-0160)
test_optics_facade_bin = executable(
  'test_optics_facade',
  files('test_optics_facade.c'),
  dependencies : optics_dep,
)

test(
  'optics_facade',
  test_optics_facade_bin,
  suite : 'optics',
)
