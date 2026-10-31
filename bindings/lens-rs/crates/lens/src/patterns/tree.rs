//! TreeView pattern.

use crate::Frame;
use std::ffi::CString;

/// Item configuration for TreeView.
#[derive(Clone, Debug)]
pub struct TreeItem {
    pub label: String,
    pub icon: lens_sys::lens_icon_id,
    pub depth: u32,
    pub is_leaf: bool,
    pub expanded: bool,
    pub selected: bool,
    pub badge: Option<String>,
}

impl TreeItem {
    pub fn new(label: impl Into<String>) -> Self {
        Self {
            label: label.into(),
            icon: lens_sys::lens_icon_id(0),
            depth: 0,
            is_leaf: false,
            expanded: false,
            selected: false,
            badge: None,
        }
    }

    pub fn depth(mut self, depth: u32) -> Self {
        self.depth = depth;
        self
    }

    pub fn leaf(mut self, is_leaf: bool) -> Self {
        self.is_leaf = is_leaf;
        self
    }

    pub fn expanded(mut self, expanded: bool) -> Self {
        self.expanded = expanded;
        self
    }

    pub fn selected(mut self, selected: bool) -> Self {
        self.selected = selected;
        self
    }

    pub fn icon(mut self, icon: lens_sys::lens_icon_id) -> Self {
        self.icon = icon;
        self
    }

    pub fn badge(mut self, badge: impl Into<String>) -> Self {
        self.badge = Some(badge.into());
        self
    }
}

/// Action resulting from tree view interaction.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TreeAction {
    None,
    Select(usize),
    Toggle(usize),
    Activate(usize),
}

/// Builder for TreeView presentation.
#[derive(Clone, Debug)]
pub struct TreeView {
    id: String,
    height: f32,
    row_height: f32,
    indent_per_level: f32,
}

impl TreeView {
    pub fn new(id: impl Into<String>) -> Self {
        Self {
            id: id.into(),
            height: 0.0,
            row_height: 26.0,
            indent_per_level: 16.0,
        }
    }

    pub fn height(mut self, h: f32) -> Self {
        self.height = h;
        self
    }

    pub fn row_height(mut self, h: f32) -> Self {
        self.row_height = h;
        self
    }

    pub fn indent_per_level(mut self, indent: f32) -> Self {
        self.indent_per_level = indent;
        self
    }

    pub fn show(&self, frame: &mut Frame, items: &mut [TreeItem]) -> TreeAction {
        let id_c = CString::new(self.id.as_str()).unwrap_or_default();
        let label_cstrings: Vec<CString> = items
            .iter()
            .map(|it| CString::new(it.label.as_str()).unwrap_or_default())
            .collect();

        let badge_cstrings: Vec<Option<CString>> = items
            .iter()
            .map(|it| it.badge.as_deref().and_then(|b| CString::new(b).ok()))
            .collect();

        let mut raw_items: Vec<lens_sys::lens_tree_item> = items
            .iter()
            .enumerate()
            .map(|(i, it)| lens_sys::lens_tree_item {
                label: label_cstrings[i].as_ptr(),
                icon: it.icon,
                depth: it.depth,
                is_leaf: it.is_leaf,
                expanded: it.expanded,
                selected: it.selected,
                badge: badge_cstrings[i]
                    .as_ref()
                    .map(|cs| cs.as_ptr())
                    .unwrap_or(std::ptr::null()),
            })
            .collect();

        let raw_opts = lens_sys::lens_tree_opts {
            height: self.height,
            row_height: self.row_height,
            indent_per_level: self.indent_per_level,
        };

        let act = unsafe {
            lens_sys::lens_tree_view(
                frame.as_raw(),
                id_c.as_ptr(),
                raw_items.as_mut_ptr(),
                raw_items.len() as u32,
                &raw_opts,
            )
        };

        // Copy back expanded state
        for (it, raw) in items.iter_mut().zip(&raw_items) {
            it.expanded = raw.expanded;
        }

        match act.kind {
            lens_sys::lens_tree_action_kind::LENS_TREE_ACTION_SELECT => {
                TreeAction::Select(act.index as usize)
            }
            lens_sys::lens_tree_action_kind::LENS_TREE_ACTION_TOGGLE => {
                TreeAction::Toggle(act.index as usize)
            }
            lens_sys::lens_tree_action_kind::LENS_TREE_ACTION_ACTIVATE => {
                TreeAction::Activate(act.index as usize)
            }
            _ => TreeAction::None,
        }
    }
}
