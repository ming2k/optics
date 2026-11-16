# Headless tests for aperture, the lens ↔ iris application shell.
#
# aperture.c is compiled directly so the test can reach the host binding
# (hidden visibility) and drive it the way a platform backend does, without a
# compositor.
exe_binding = executable('test_host_binding',
  ['test_host_binding.c', '../../libs/aperture/src/aperture.c'],
  include_directories : [aperture_inc, include_directories('../../libs/aperture/src')],
  c_args              : ['-DAPERTURE_STATIC=1'],
  dependencies        : [iris_dep, lens_dep, m_dep, optics_test_dep],
)
test('test_host_binding', exe_binding, suite : 'unit')
