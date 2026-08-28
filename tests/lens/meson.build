tests = ['test_id', 'test_store', 'test_layout', 'test_input',
         'test_reconcile', 'test_widgets', 'test_render_null',
         'test_a11y', 'test_clipboard', 'test_place', 'test_place_bands',
         'test_input_extended', 'test_textfield', 'test_stress',
         'test_font_fallback', 'test_disabled', 'test_validation',
         'test_tooltip', 'test_dropdown', 'test_radio', 'test_tabs',
         'test_textarea', 'test_button', 'test_checkbox', 'test_slider',
         'test_progress', 'test_scroll', 'test_selectable', 'test_modal',
         'test_menu', 'test_split', 'test_table', 'test_pressable',
         'test_place_occlusion', 'test_scroll_hit_clip',
         'test_reduced_motion', 'test_tree', 'test_repaint',
         'test_record_replay', 'test_icon_runtime', 'test_label_centering',
         'test_button_centering',
         'test_opacity',
         'test_ghost',
         'test_skin', 'test_audit_fixes', 'test_a11y_activate',
         'test_store_spike', 'test_abi_guards', 'test_skin_userdata', 'test_user_widget_kind',
         'test_icon_info']

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

# style.c is compiled into the test binary directly (same pattern as
# test_drawlist_hash): lensi_style_resolve is a hidden symbol, so the
# resolver unit tests link their own copy.
test_state_style = executable('test_state_style',
  'test_state_style.c',
  '../../libs/lens/src/style/style.c',
  dependencies        : [lens_dep, m_dep],
  include_directories : lens_inc,
)
test('state_style', test_state_style)

# Same pattern as test_state_style: test_text_scale asserts the resolved
# font_size carries the accessibility text factor, so it links style.c too.
test_text_scale = executable('test_text_scale',
  'test_text_scale.c',
  '../../libs/lens/src/style/style.c',
  dependencies        : [lens_dep, m_dep],
  include_directories : lens_inc,
)
test('text_scale', test_text_scale)

# Same pattern as test_state_style: the cascade helpers (merge/effective)
# are hidden symbols, so the test links its own copy of style.c.
test_style_cascade = executable('test_style_cascade',
  'test_style_cascade.c',
  '../../libs/lens/src/style/style.c',
  dependencies        : [lens_dep, m_dep],
  include_directories : lens_inc,
)
test('style_cascade', test_style_cascade)

# Content of registered runtime-icon streams. Compiles the registration
# sources directly (same pattern as test_drawlist_hash): the registry
# accessors are hidden symbols that do not link from liblens.so.
test_icon_runtime_content = executable('test_icon_runtime_content',
  'test_icon_runtime_content.c',
  '../../libs/lens/src/icon_runtime.c',
  '../../libs/lens/src/icon_data.c',
  '../../libs/lens/src/vendor/nanosvg.c',
  dependencies        : [lens_dep, m_dep],
  include_directories : lens_inc,
)
test('icon_runtime_content', test_icon_runtime_content)

# Per-shape colour runs recorded by runtime SVG registration. Same
# direct-link pattern as test_icon_runtime_content.
test_icon_runtime_runs = executable('test_icon_runtime_runs',
  'test_icon_runtime_runs.c',
  '../../libs/lens/src/icon_runtime.c',
  '../../libs/lens/src/icon_data.c',
  '../../libs/lens/src/vendor/nanosvg.c',
  dependencies        : [lens_dep, m_dep],
  include_directories : lens_inc,
)
test('icon_runtime_runs', test_icon_runtime_runs)

if get_option('fuzz')
  subdir('fuzz')
endif
