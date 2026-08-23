# tests/anim — property tests for the motion vocabulary. CPU-only; no
# Vulkan, no canvas, no lens: the library is pure math.
if get_option('anim')
  test_anim = executable('test_anim', 'test_anim.c',
    dependencies : [anim_dep, m_dep],
    include_directories : anim_inc,
  )
  test('anim', test_anim)
endif
