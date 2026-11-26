//! Segmented control pattern.

use crate::Frame;
use std::ffi::CString;

/// An item in a segmented control.
#[derive(Clone, Debug)]
pub struct SegmentedItem {
    pub label: Option<String>,
    pub icon: optics_ui_sys::optics_ui_icon_id,
    pub disabled: bool,
}

impl SegmentedItem {
    pub fn text(label: impl Into<String>) -> Self {
        Self {
            label: Some(label.into()),
            icon: optics_ui_sys::optics_ui_icon_id::OPTICS_UI_ICON_NONE,
            disabled: false,
        }
    }

    pub fn icon(icon: optics_ui_sys::optics_ui_icon_id) -> Self {
        Self {
            label: None,
            icon,
            disabled: false,
        }
    }

    pub fn icon_text(icon: optics_ui_sys::optics_ui_icon_id, label: impl Into<String>) -> Self {
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
            .map(|item| {
                item.label
                    .as_ref()
                    .map(|s| CString::new(s.as_str()).unwrap_or_default())
            })
            .collect();

        let c_items: Vec<optics_ui_sys::optics_ui_segmented_item> = items
            .iter()
            .enumerate()
            .map(|(i, item)| optics_ui_sys::optics_ui_segmented_item {
                label: c_labels[i]
                    .as_ref()
                    .map_or(std::ptr::null(), |c| c.as_ptr()),
                icon: item.icon,
                disabled: item.disabled,
            })
            .collect();

        let mut raw_sel = *selected_index as u32;
        let opts = optics_ui_sys::optics_ui_segmented_opts {
            id: id_c.as_ptr(),
            items: c_items.as_ptr(),
            item_count: c_items.len() as u32,
            selected_index: &mut raw_sel,
            height: self.height,
            min_item_width: self.min_item_width,
            compact: self.compact,
        };

        let changed = unsafe { optics_ui_sys::optics_ui_segmented_control(frame.as_raw(), &opts) };

        if changed {
            *selected_index = raw_sel as usize;
        }

        changed
    }
}
