//! Interactive split pane divider pattern.

use crate::Frame;
use std::ffi::CString;

/// Options for split pane divider.
#[derive(Clone, Copy, Debug)]
pub struct SplitOpts {
    pub min_size: f32,
    pub max_size: f32,
    pub handle_width: f32,
    pub hit_expand: f32,
}

impl Default for SplitOpts {
    fn default() -> Self {
        Self {
            min_size: 160.0,
            max_size: 480.0,
            handle_width: 2.0,
            hit_expand: 4.0,
        }
    }
}

/// Interactive vertical split divider helper.
pub fn split_handle_v(
    frame: &mut Frame,
    id: &str,
    split_offset: &mut f32,
    opts: &SplitOpts,
) -> bool {
    let id_c = CString::new(id).unwrap_or_default();
    let raw_opts = lens_sys::lens_split_opts {
        min_size: opts.min_size,
        max_size: opts.max_size,
        handle_width: opts.handle_width,
        hit_expand: opts.hit_expand,
    };

    unsafe {
        lens_sys::lens_split_handle_v(
            frame.as_raw(),
            id_c.as_ptr(),
            split_offset as *mut f32,
            &raw_opts,
        )
    }
}
