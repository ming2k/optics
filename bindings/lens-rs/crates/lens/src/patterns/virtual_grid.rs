//! Virtual layout math helpers for list/grid virtualization.

pub use lens_sys::lens_virtual_grid_plan as VirtualGridPlan;

/// Compute virtual grid slice metrics.
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
    unsafe {
        lens_sys::lens_virtual_grid_calc(
            available_width,
            viewport_height,
            scroll_y,
            total_items as u32,
            min_col_width,
            max_col_width,
            item_height,
            target_gap,
            overscan_rows as u32,
        )
    }
}
