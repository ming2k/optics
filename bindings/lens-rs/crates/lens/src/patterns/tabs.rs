//! Tab strip pattern.

use crate::Frame;
use std::ffi::CString;

/// Tab item description.
#[derive(Clone, Debug)]
pub struct TabItem {
    pub title: String,
    pub icon: lens_sys::lens_icon_id,
    pub closable: bool,
}

impl TabItem {
    pub fn new(title: impl Into<String>) -> Self {
        Self {
            title: title.into(),
            icon: lens_sys::lens_icon_id(u32::MAX),
            closable: true,
        }
    }

    pub fn icon(mut self, icon: lens_sys::lens_icon_id) -> Self {
        self.icon = icon;
        self
    }

    pub fn closable(mut self, closable: bool) -> Self {
        self.closable = closable;
        self
    }
}

/// Action resulting from tab strip interaction.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TabAction {
    None,
    Select(usize),
    Close(usize),
    NewTab,
}

/// Builder for tab strip presentation.
#[derive(Clone, Debug)]
pub struct TabStrip {
    id: String,
    height: f32,
    min_tab_width: f32,
    max_tab_width: f32,
    show_new_button: bool,
    close_icon: lens_sys::lens_icon_id,
    new_icon: lens_sys::lens_icon_id,
}

impl TabStrip {
    pub fn new(id: impl Into<String>) -> Self {
        Self {
            id: id.into(),
            height: 36.0,
            min_tab_width: 80.0,
            max_tab_width: 200.0,
            show_new_button: true,
            close_icon: lens_sys::lens_icon_id(u32::MAX),
            new_icon: lens_sys::lens_icon_id(u32::MAX),
        }
    }

    pub fn height(mut self, h: f32) -> Self {
        self.height = h;
        self
    }

    pub fn min_tab_width(mut self, min_w: f32) -> Self {
        self.min_tab_width = min_w;
        self
    }

    pub fn max_tab_width(mut self, max_w: f32) -> Self {
        self.max_tab_width = max_w;
        self
    }

    pub fn show_new_button(mut self, show: bool) -> Self {
        self.show_new_button = show;
        self
    }

    pub fn close_icon(mut self, icon: lens_sys::lens_icon_id) -> Self {
        self.close_icon = icon;
        self
    }

    pub fn new_icon(mut self, icon: lens_sys::lens_icon_id) -> Self {
        self.new_icon = icon;
        self
    }

    /// Show the tab strip and return any action triggered this frame.
    pub fn show(
        &self,
        frame: &mut Frame,
        tabs: &[TabItem],
        active_index: usize,
    ) -> TabAction {
        let id_c = CString::new(self.id.as_str()).unwrap_or_default();
        let c_titles: Vec<CString> = tabs
            .iter()
            .map(|t| CString::new(t.title.as_str()).unwrap_or_default())
            .collect();

        let c_tabs: Vec<lens_sys::lens_tab_item> = tabs
            .iter()
            .enumerate()
            .map(|(i, t)| lens_sys::lens_tab_item {
                title: c_titles[i].as_ptr(),
                icon: t.icon,
                closable: t.closable,
            })
            .collect();

        let opts = lens_sys::lens_tab_strip_opts {
            height: self.height,
            min_tab_width: self.min_tab_width,
            max_tab_width: self.max_tab_width,
            show_new_button: self.show_new_button,
            close_icon: self.close_icon,
            new_icon: self.new_icon,
        };

        let raw_action = unsafe {
            lens_sys::lens_tab_strip(
                frame.as_raw(),
                id_c.as_ptr(),
                c_tabs.as_ptr(),
                c_tabs.len() as u32,
                active_index as u32,
                &opts,
            )
        };

        match raw_action.kind {
            lens_sys::lens_tab_action_kind::LENS_TAB_ACTION_SELECT => {
                TabAction::Select(raw_action.index as usize)
            }
            lens_sys::lens_tab_action_kind::LENS_TAB_ACTION_CLOSE => {
                TabAction::Close(raw_action.index as usize)
            }
            lens_sys::lens_tab_action_kind::LENS_TAB_ACTION_NEW => TabAction::NewTab,
            _ => TabAction::None,
        }
    }
}
