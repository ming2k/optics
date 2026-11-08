//! Menu & MenuBar pattern.

use crate::Frame;
use crate::types::Rect;
use std::ffi::CString;

/// Item in a popup menu.
#[derive(Clone, Debug)]
pub struct MenuItem {
    pub label: String,
    pub icon: lens_sys::lens_icon_id,
    pub shortcut: Option<String>,
    pub disabled: bool,
    pub is_separator: bool,
}

impl MenuItem {
    pub fn new(label: impl Into<String>) -> Self {
        Self {
            label: label.into(),
            icon: lens_sys::lens_icon_id(0),
            shortcut: None,
            disabled: false,
            is_separator: false,
        }
    }

    pub fn separator() -> Self {
        Self {
            label: String::new(),
            icon: lens_sys::lens_icon_id(0),
            shortcut: None,
            disabled: false,
            is_separator: true,
        }
    }

    pub fn icon(mut self, icon: lens_sys::lens_icon_id) -> Self {
        self.icon = icon;
        self
    }

    pub fn shortcut(mut self, sc: impl Into<String>) -> Self {
        self.shortcut = Some(sc.into());
        self
    }

    pub fn disabled(mut self, disabled: bool) -> Self {
        self.disabled = disabled;
        self
    }
}

/// Menubar top-level category item.
#[derive(Clone, Debug)]
pub struct MenubarItem {
    pub title: String,
    pub menu_id: String,
}

impl MenubarItem {
    pub fn new(title: impl Into<String>, menu_id: impl Into<String>) -> Self {
        Self {
            title: title.into(),
            menu_id: menu_id.into(),
        }
    }
}

/// Render a popup menu anchored to a rect. Returns activated index (if any).
pub fn popup_menu(
    frame: &mut Frame,
    menu_id: &str,
    anchor: Rect,
    items: &[MenuItem],
) -> Option<usize> {
    let id_c = CString::new(menu_id).unwrap_or_default();
    let label_cstrings: Vec<CString> = items
        .iter()
        .map(|it| CString::new(it.label.as_str()).unwrap_or_default())
        .collect();

    let shortcut_cstrings: Vec<Option<CString>> = items
        .iter()
        .map(|it| it.shortcut.as_deref().and_then(|s| CString::new(s).ok()))
        .collect();

    let raw_items: Vec<lens_sys::lens_menu_item> = items
        .iter()
        .enumerate()
        .map(|(i, it)| lens_sys::lens_menu_item {
            label: label_cstrings[i].as_ptr(),
            icon: it.icon,
            shortcut: shortcut_cstrings[i]
                .as_ref()
                .map(|cs| cs.as_ptr())
                .unwrap_or(std::ptr::null()),
            disabled: it.disabled,
            is_separator: it.is_separator,
        })
        .collect();

    let raw_opts = lens_sys::lens_menu_opts {
        width: 180.0,
        item_height: 28.0,
    };

    let act = unsafe {
        lens_sys::lens_popup_menu(
            frame.as_raw(),
            id_c.as_ptr(),
            anchor.to_raw(),
            raw_items.as_ptr(),
            raw_items.len() as u32,
            &raw_opts,
        )
    };

    if act >= 0 { Some(act as usize) } else { None }
}

/// Render a horizontal menu bar. Returns the active open menu index (if any) and its anchor rect.
pub fn menubar(
    frame: &mut Frame,
    id: &str,
    items: &[MenubarItem],
) -> (Option<usize>, Option<Rect>) {
    let id_c = CString::new(id).unwrap_or_default();
    let title_cstrings: Vec<CString> = items
        .iter()
        .map(|it| CString::new(it.title.as_str()).unwrap_or_default())
        .collect();

    let menu_id_cstrings: Vec<CString> = items
        .iter()
        .map(|it| CString::new(it.menu_id.as_str()).unwrap_or_default())
        .collect();

    let raw_items: Vec<lens_sys::lens_menubar_item> = items
        .iter()
        .enumerate()
        .map(|(i, _)| lens_sys::lens_menubar_item {
            title: title_cstrings[i].as_ptr(),
            menu_id: menu_id_cstrings[i].as_ptr(),
        })
        .collect();

    let mut anchor_rect = lens_sys::flux_rect {
        x: 0.0,
        y: 0.0,
        w: 0.0,
        h: 0.0,
    };

    let open_idx = unsafe {
        lens_sys::lens_menubar(
            frame.as_raw(),
            id_c.as_ptr(),
            raw_items.as_ptr(),
            raw_items.len() as u32,
            &mut anchor_rect,
        )
    };

    if open_idx >= 0 {
        (Some(open_idx as usize), Some(Rect::from_raw(anchor_rect)))
    } else {
        (None, None)
    }
}
