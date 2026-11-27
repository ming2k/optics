# Umbrella façade, version lockstep, and essential tree-level lint checks.
py = import('python').find_installation('python3')

# One versioning scheme across every library and the umbrella facade. The
# runtime accessors are exercised here; the tree-level script below also checks
# Meson and the Cargo workspace, which a C binary cannot see.
version_parts = meson.project_version().split('.')
version_args = [
  '-DOPTICS_PROJECT_VERSION_MAJOR=' + version_parts[0],
  '-DOPTICS_PROJECT_VERSION_MINOR=' + version_parts[1],
  '-DOPTICS_PROJECT_VERSION_PATCH=' + version_parts[2],
]
test_version_lockstep = executable('test_version_lockstep',
  'test_version_lockstep.c',
  dependencies : [optics_dep, optics_test_dep],
  c_args : version_args,
)
test('version_lockstep', test_version_lockstep, suite : 'integration')

# Wayland listener slot contract (ADR-0145): libwayland aborts if any
# dispatched event hits a NULL slot, and C compilers do not warn on omitted
# fields in designated initializers.
test(
  'wayland_listeners',
  py,
  args : [files('../../tools/check_listeners.py')],
  workdir : meson.project_source_root(),
  suite : 'lint',
  timeout : 30,
)

# Version lockstep across Meson, the C headers, the umbrella header, and the
# Cargo workspace.
test(
  'version_lockstep_tree',
  py,
  args : [files('../../tools/check_versions.py')],
  workdir : meson.project_source_root(),
  suite : 'lint',
  timeout : 30,
)

# Shared libraries never write to stdout; gfx never references named materials.
test(
  'library_purity',
  py,
  args : [files('../../tools/check_library_purity.py')],
  workdir : meson.project_source_root(),
  suite : 'lint',
  timeout : 30,
)

# Verify docs/reference/symbols.md stays in sync with public headers.
test(
  'symbol_reference',
  py,
  args : [files('../../tools/gen_symbols.py'), '--check'],
  workdir : meson.project_source_root(),
  suite : 'lint',
  timeout : 30,
)

# Umbrella Facade and Integrated Dependencies (ADR-0160). The facade test
# exercises ui/window through the umbrella, so it is meaningless when
# the selected distribution omits ui.
if opt_ui
  test_optics_facade_bin = executable(
    'test_optics_facade',
    files('test_optics_facade.c'),
    dependencies : [optics_dep, optics_test_dep],
  )

  test(
    'optics_facade',
    test_optics_facade_bin,
    suite : 'integration',
  )
endif
