# tests/e2e — the top of the pyramid.
#
# Rules for this directory (enforced by tools/check_test_layers.py):
#   - public headers only: no libs/*/src include, no internal.h, no compiling
#     a library source file into the test binary;
#   - one process-level scenario per binary, linked through the umbrella
#     façade (optics_dep) and the shared assertion harness;
#   - deterministic and headless: CPU canvas, no compositor, no GPU.
#
# E2E tests are intentionally few. A test that needs a private header or a
# compiled-in implementation belongs in unit or integration.

e2e_common_deps = [optics_dep, optics_test_dep, m_dep]

# Always present: the umbrella/gfx version contract works in every feature
# configuration, including a gfx-only distribution.
test_e2e_gfx_version = executable('test_e2e_gfx_version',
  'test_e2e_gfx_version.c',
  dependencies : e2e_common_deps,
)
test('e2e_gfx_version', test_e2e_gfx_version, suite : 'e2e')

if opt_ui
  test_e2e_public_api = executable('test_e2e_public_api',
    'test_e2e_public_api.c',
    dependencies : e2e_common_deps,
  )
  test('e2e_public_api', test_e2e_public_api, suite : 'e2e')

  test_e2e_interaction_session = executable('test_e2e_interaction_session',
    'test_e2e_interaction_session.c',
    dependencies : e2e_common_deps,
  )
  test('e2e_interaction_session', test_e2e_interaction_session, suite : 'e2e')

  if opt_svg
    test_e2e_document_pipeline = executable('test_e2e_document_pipeline',
      'test_e2e_document_pipeline.c',
      dependencies : e2e_common_deps,
    )
    test('e2e_document_pipeline', test_e2e_document_pipeline, suite : 'e2e')
  endif
endif
