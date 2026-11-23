# Headless tests for app, the ui ↔ window application shell.
#
# app.c is compiled directly so the test can reach the host binding
# (hidden visibility) and drive it the way a platform backend does, without a
# compositor.
exe_binding = executable('test_host_binding',
  ['test_host_binding.c', '../../libs/app/src/app.c'],
  include_directories : [app_inc, include_directories('../../libs/app/src')],
  c_args              : ['-DOPTICS_APP_STATIC=1'],
  dependencies        : [window_dep, ui_dep, m_dep, optics_test_dep],
)
test('test_host_binding', exe_binding, suite : 'unit')
