tests = ['test_id', 'test_store', 'test_layout', 'test_input',
         'test_reconcile', 'test_widgets', 'test_render_null',
         'test_a11y', 'test_clipboard', 'test_overlay', 'test_layer',
         'test_input_extended', 'test_textfield', 'test_stress',
         'test_font_fallback', 'test_disabled', 'test_validation',
         'test_tooltip', 'test_dropdown', 'test_radio', 'test_tabs',
         'test_textarea', 'test_button', 'test_checkbox', 'test_slider',
         'test_progress', 'test_scroll', 'test_selectable', 'test_modal',
         'test_menu', 'test_split', 'test_table', 'test_pressable',
         'test_overlay_eclipse', 'test_scroll_hit_clip',
         'test_reduced_motion', 'test_tree', 'test_repaint',
         'test_record_replay', 'test_icon_runtime']

foreach t : tests
  exe = executable(t, t + '.c',
    dependencies        : [lens_dep, m_dep],
    include_directories : lens_inc,
  )
  test(t, exe)
endforeach

# drawlist.c is compiled into the test binary directly (same pattern
# as test_canvas_tess): lensi_drawlist_push is hidden in liblens, so
# the test needs its own copy to push scripted commands onto a live
# node and observe subtree_changed across frames.
test_drawlist_hash = executable('test_drawlist_hash',
  'test_drawlist_hash.c',
  '../../libs/lens/src/render/drawlist.c',
  dependencies        : [lens_dep, m_dep],
  include_directories : lens_inc,
)
test('drawlist_hash', test_drawlist_hash)

if get_option('fuzz')
  subdir('fuzz')
endif
