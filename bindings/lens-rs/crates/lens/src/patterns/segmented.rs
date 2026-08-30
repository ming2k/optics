//! Segmented control pattern.

use crate::Frame;
use std::ffi::CString;

/// An item in a segmented control.
#[derive(Clone, Debug)]
pub struct SegmentedItem {
    pub label: Option<String>,
    pub icon: lens_sys::lens_icon_id,
    pub disabled: bool,
}

impl SegmentedItem {
    pub fn text(label: impl Into<String>) -> Self {
        Self {
            label: Some(label.into()),
            icon: lens_sys::lens_icon_id(u32::MAX), // invalid icon
            disabled: false,
        }
    }

    pub fn icon(icon: lens_sys::lens_icon_id) -> Self {
        Self {
            label: None,
            icon,
            disabled: false,
        }
    }

    pub fn icon_text(icon: lens_sys::lens_icon_id, label: impl Into<String>) -> Self {
        Self {
            label: Some(label.into()),
            icon,
            disabled: false,
        }
    }
}

/// Builder for segmented control presentation.
#[derive(Clone, Debug)]
pub struct SegmentedControl {
    id: String,
    height: f32,
    min_item_width: f32,
    compact: bool,
}

impl SegmentedControl {
    pub fn new(id: impl Into<String>) -> Self {
        Self {
            id: id.into(),
            height: 0.0,
            min_item_width: 0.0,
            compact: true,
        }
    }

    pub fn height(mut self, height: f32) -> Self {
        self.height = height;
        self
    }

    pub fn min_item_width(mut self, min_w: f32) -> Self {
        self.min_item_width = min_w;
        self
    }

    pub fn compact(mut self, compact: bool) -> Self {
        self.compact = compact;
        self
    }

    /// Show the segmented control and return whether the selection changed.
    pub fn show(
        &self,
        frame: &mut Frame,
        items: &[SegmentedItem],
        selected_index: &mut usize,
    ) -> bool {
        let id_c = CString::new(self.id.as_str()).unwrap_or_default();
        let c_labels: Vec<Option<CString>> = items
            .iter()
            .map(|item| item.label.as_ref().map(|s| CString::new(s.as_str()).unwrap_or_default()))
            .collect();

        let c_items: Vec<lens_sys::lens_segmented_item> = items
            .iter()
            .enumerate()
            .map(|(i, item)| lens_sys::lens_segmented_item {
                label: c_labels[i].as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
                icon: item.icon,
                disabled: item.disabled,
            })
            .collect();

        let opts = lens_sys::lens_segmented_opts {
            height: self.height,
            min_item_width: self.min_item_width,
            compact: self.compact,
        };

        let mut raw_sel = *selected_index as u32;
        let changed = unsafe {
            lens_sys::lens_segmented_control(
                frame.as_raw(),
                id_c.as_ptr(),
                c_items.as_ptr(),
                c_items.len() as u32,
                &mut raw_sel,
                &opts,
            )
        };

        if changed {
            *selected_index = raw_sel as usize;
        }

        changed
    }
}
