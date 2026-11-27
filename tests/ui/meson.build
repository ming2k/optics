tests = ['test_id', 'test_store', 'test_layout', 'test_input',
         'test_reconcile', 'test_widgets', 'test_render_null',
         'test_a11y', 'test_clipboard', 'test_place', 'test_place_bands',
         'test_input_extended', 'test_textedit', 'test_stress',
         'test_font_fallback', 'test_disabled', 'test_validation',
         'test_tooltip', 'test_radio',
         'test_button', 'test_checkbox', 'test_slider',
         'test_scroll', 'test_selectable', 'test_pressable',
         'test_place_occlusion', 'test_scroll_hit_clip',
         'test_reduced_motion', 'test_motion', 'test_repaint',
         'test_record_replay', 'test_icon_runtime', 'test_label_centering',
         'test_button_centering',
         'test_opacity',
         'test_clip_scope',
         'test_ghost',
         'test_skin', 'test_audit_fixes', 'test_a11y_activate',
         'test_store_spike', 'test_abi_guards', 'test_skin_userdata', 'test_user_widget_kind',
         'test_icon_info', 'test_dnd', 'test_tab_strip',
         'test_datagrid', 'test_tree_view', 'test_menu',
         # The zero-allocation frame walk, enforced by allocator interposition.
         'test_zero_alloc']

foreach t : tests
  exe = executable(t, t + '.c',
    dependencies        : [ui_dep, m_dep, optics_test_dep],
    include_directories : ui_inc,
  )
  test(t, exe, suite : 'unit')
endforeach

# drawlist.c is compiled into the test binary directly (same pattern
# as test_canvas_tess): optics_ui__drawlist_push is hidden in liboptics_ui, so
# the test needs its own copy to push scripted commands onto a live
# node and observe subtree_changed across frames.
test_drawlist_hash = executable('test_drawlist_hash',
  'test_drawlist_hash.c',
  '../../libs/ui/src/render/drawlist.c',
  dependencies        : [ui_dep, m_dep, optics_test_dep],
  include_directories : ui_inc,
)
test('drawlist_hash', test_drawlist_hash, suite : 'unit')

# style.c is compiled into the test binary directly (same pattern as
# test_drawlist_hash): optics_ui__style_resolve is a hidden symbol, so the
# resolver unit tests link their own copy.
test_state_style = executable('test_state_style',
  'test_state_style.c',
  '../../libs/ui/src/style/style.c',
  dependencies        : [ui_dep, m_dep, optics_test_dep],
  include_directories : ui_inc,
)
test('state_style', test_state_style, suite : 'unit')

# Same pattern as test_state_style: test_text_scale asserts the resolved
# font_size carries the accessibility text factor, so it links style.c too.
test_text_scale = executable('test_text_scale',
  'test_text_scale.c',
  '../../libs/ui/src/style/style.c',
  dependencies        : [ui_dep, m_dep, optics_test_dep],
  include_directories : ui_inc,
)
test('text_scale', test_text_scale, suite : 'unit')

# Same pattern as test_state_style: the cascade helpers (merge/effective)
# are hidden symbols, so the test links its own copy of style.c.
test_style_cascade = executable('test_style_cascade',
  'test_style_cascade.c',
  '../../libs/ui/src/style/style.c',
  dependencies        : [ui_dep, m_dep, optics_test_dep],
  include_directories : ui_inc,
)
test('style_cascade', test_style_cascade, suite : 'unit')

# ADR-0084 borrow-checker self-test. The registry accessors are hidden
# symbols, so this test links borrow.c directly (same direct-link
# pattern as test_icon_runtime_content); the live registry in the
# shared library still runs during the in-process legitimate-pattern
# section. The violation case forks and expects SIGABRT.
# The violation case forks and re-execs /proc/self/exe; keep it on Linux.
if host_machine.system() == 'linux'
  test_borrow_check = executable('test_borrow_check',
    'test_borrow_check.c',
    '../../libs/ui/src/core/borrow.c',
    dependencies        : [ui_dep, m_dep, optics_test_dep],
    include_directories : ui_inc,
    c_args              : ['-DOPTICS_UI_DEBUG_BORROWS=1'],
  )
  test('borrow_check', test_borrow_check, suite : 'unit')
endif

# Icon coverage-mask cache (ADR-0112): bake sizing, key identity, layer
# separation, device-grid-exact placement at fractional scales, and the run
# resolver that keeps the mask path and the vector fallback in agreement.
test_icon_mask = executable('test_icon_mask',
  'test_icon_mask.c',
  '../../libs/ui/src/render/icon_mask.c',
  '../../libs/ui/src/icon_runtime.c',
  dependencies        : [ui_dep, m_dep, optics_test_dep],
  include_directories : [ui_inc, gfx_inc],
  c_args              : ['-DOPTICS_UI_STATIC=1'],
)
test('icon_mask', test_icon_mask, suite : 'unit')

# End-to-end runtime-icon rendering through the PUBLIC API only (ADR-0112).
test_icon_mask_render = executable('test_icon_mask_render',
  'test_icon_mask_render.c',
  dependencies        : [ui_dep, m_dep, optics_test_dep],
  include_directories : ui_inc,
)
test('icon_mask_render', test_icon_mask_render, suite : 'integration')

if get_option('fuzz')
  subdir('fuzz')
endif

contract_test = executable('test_contracts', 'test_contracts.c', dependencies : [ui_dep, m_dep, optics_test_dep])
test('ui_contracts', contract_test, suite : 'unit')
store_fault_test = executable('test_store_fault', 'test_store_fault.c',
  '../../libs/ui/src/store/store.c', dependencies : [ui_dep, m_dep, optics_test_dep])
test('ui_store_fault', store_fault_test, suite : 'unit')
# Model-based store stress (store_model.h): store.c compiled in, checked
# against a reference model and its structural invariants every frame. A
# store bug that loops forever fails on the timeout instead of hanging CI.
store_model_test = executable('test_store_model', 'test_store_model.c',
  '../../libs/ui/src/store/store.c', dependencies : [ui_dep, m_dep, optics_test_dep])
test('ui_store_model', store_model_test, suite : 'unit', timeout : 120)

snapshot_test = executable('test_snapshot', 'test_snapshot.c', dependencies : [ui_dep, m_dep, optics_test_dep])
test('ui_snapshot', snapshot_test, suite : 'integration')

ui_gpu_render_test = executable('test_ui_gpu_render', 'test_ui_gpu_render.c',
  dependencies : [optics_gfx_test_support_dep, ui_dep],
)
test('ui_gpu_render', ui_gpu_render_test, suite : 'integration')
