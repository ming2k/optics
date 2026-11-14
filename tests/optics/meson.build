# Code quality and tooling verification
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
if opt_glyph
  version_args += '-DOPTICS_TEST_WITH_GLYPH=1'
endif
if opt_transit
  version_args += '-DOPTICS_TEST_WITH_TRANSIT=1'
endif
if opt_vista
  version_args += '-DOPTICS_TEST_WITH_VISTA=1'
endif
if get_option('prism')
  version_args += '-DOPTICS_TEST_WITH_PRISM=1'
endif
if get_option('reticle')
  version_args += '-DOPTICS_TEST_WITH_RETICLE=1'
endif
test_version_lockstep = executable('test_version_lockstep',
  'test_version_lockstep.c',
  dependencies : [optics_dep, optics_test_dep],
  c_args : version_args,
)
test('version_lockstep', test_version_lockstep, suite : 'integration')

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

# Version lockstep across Meson, the C headers, the umbrella header, and the
# Cargo workspace. The runtime accessor test cannot see Meson/Cargo, and Cargo
# cannot see the C headers; this is the tree-level gate.
test(
  'version_lockstep_tree',
  py,
  args : [files('../../tools/check_versions.py')],
  workdir : meson.project_source_root(),
  suite : 'lint',
  timeout : 30,
)

test(
  'library_purity',
  py,
  args : [files('../../tools/check_library_purity.py')],
  workdir : meson.project_source_root(),
  suite : 'lint',
  timeout : 30,
)

test(
  'docs_links',
  py,
  args : [files('../../tools/check_doc_links.py')],
  workdir : meson.project_source_root(),
  suite : 'lint',
  timeout : 30,
)

# The test pyramid is a mechanical taxonomy, not a convention: every test must
# declare exactly one layer (unit/integration/e2e) or an orthogonal lane
# (bench/fuzz), names are globally unique, and e2e sources may not reach into
# library internals.
test(
  'test_layers',
  py,
  args : [files('../../tools/check_test_layers.py'), meson.project_build_root()],
  workdir : meson.project_source_root(),
  suite : ['lint', 'tooling'],
  timeout : 60,
)

# Negative tests for the taxonomy guardrail itself: an accepting gate that
# cannot fail is not a gate.
test(
  'test_layers_guardrail_selftest',
  py,
  args : [files('test_test_layers_guardrail.py')],
  workdir : meson.project_source_root(),
  suite : 'tooling',
  timeout : 30,
)

# Umbrella Facade and Integrated Dependencies (ADR-0160)
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
