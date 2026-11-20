//! Virtual layout math helpers for list/grid virtualization.

pub use optics_ui_sys::optics_ui_virtual_grid_plan as VirtualGridPlan;

/// Compute virtual grid slice metrics.
///
/// The parameter list is fixed by `optics_ui_virtual_grid_calc`'s C ABI, which
/// cannot be re-shaped without breaking every installed consumer; the wrapper
/// mirrors it one-for-one so the two signatures stay visibly identical. A
/// parameter struct here would be a second, divergent spelling of the same
/// call.
#[allow(clippy::too_many_arguments)]
pub fn virtual_grid_calc(
    available_width: f32,
    viewport_height: f32,
    scroll_y: f32,
    total_items: usize,
    min_col_width: f32,
    max_col_width: f32,
    item_height: f32,
    target_gap: f32,
    overscan_rows: usize,
) -> VirtualGridPlan {
    let opts = optics_ui_sys::optics_ui_virtual_grid_calc_opts {
        available_width,
        viewport_height,
        scroll_y,
        total_items: total_items as u32,
        min_col_width,
        max_col_width,
        item_height,
        target_gap,
        overscan_rows: overscan_rows as u32,
    };
    unsafe { optics_ui_sys::optics_ui_virtual_grid_calc(&opts) }
}
