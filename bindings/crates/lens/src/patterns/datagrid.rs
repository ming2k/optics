//! DataGrid pattern.

use crate::Frame;
use crate::types::Align;
use std::ffi::CString;

/// Sort direction for a datagrid column.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SortDir {
    None,
    Asc,
    Desc,
}

impl SortDir {
    fn from_raw(raw: lens_sys::lens_sort_dir) -> Self {
        match raw {
            lens_sys::lens_sort_dir::LENS_SORT_ASC => SortDir::Asc,
            lens_sys::lens_sort_dir::LENS_SORT_DESC => SortDir::Desc,
            _ => SortDir::None,
        }
    }

    fn to_raw(self) -> lens_sys::lens_sort_dir {
        match self {
            SortDir::None => lens_sys::lens_sort_dir::LENS_SORT_NONE,
            SortDir::Asc => lens_sys::lens_sort_dir::LENS_SORT_ASC,
            SortDir::Desc => lens_sys::lens_sort_dir::LENS_SORT_DESC,
        }
    }
}

/// Column configuration for DataGrid.
#[derive(Clone, Debug)]
pub struct DataGridCol {
    pub title: String,
    pub width: f32,
    pub min_width: f32,
    pub sort: SortDir,
    pub sortable: bool,
    pub resizable: bool,
    pub align: Align,
}

impl DataGridCol {
    pub fn new(title: impl Into<String>) -> Self {
        Self {
            title: title.into(),
            width: 120.0,
            min_width: 40.0,
            sort: SortDir::None,
            sortable: true,
            resizable: true,
            align: Align::Start,
        }
    }

    pub fn width(mut self, width: f32) -> Self {
        self.width = width;
        self
    }

    pub fn min_width(mut self, min: f32) -> Self {
        self.min_width = min;
        self
    }

    pub fn sortable(mut self, sortable: bool) -> Self {
        self.sortable = sortable;
        self
    }

    pub fn resizable(mut self, resizable: bool) -> Self {
        self.resizable = resizable;
        self
    }

    pub fn align(mut self, align: Align) -> Self {
        self.align = align;
        self
    }
}

/// Action resulting from datagrid interaction.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DataGridAction {
    None,
    RowClicked(usize),
    RowDoubleClicked(usize),
    SortChanged { col: usize, dir: SortDir },
    ColResized(usize),
}

/// Builder for DataGrid presentation.
#[derive(Clone, Debug)]
pub struct DataGrid {
    id: String,
    height: f32,
    header_height: f32,
    row_height: f32,
    striped: bool,
}

impl DataGrid {
    pub fn new(id: impl Into<String>) -> Self {
        Self {
            id: id.into(),
            height: 0.0,
            header_height: 32.0,
            row_height: 28.0,
            striped: true,
        }
    }

    pub fn height(mut self, h: f32) -> Self {
        self.height = h;
        self
    }

    pub fn header_height(mut self, h: f32) -> Self {
        self.header_height = h;
        self
    }

    pub fn row_height(mut self, h: f32) -> Self {
        self.row_height = h;
        self
    }

    pub fn striped(mut self, striped: bool) -> Self {
        self.striped = striped;
        self
    }

    pub fn show(
        &self,
        frame: &mut Frame,
        cols: &mut [DataGridCol],
        cell_texts: &[&str],
        row_count: usize,
        selected_row: &mut usize,
    ) -> DataGridAction {
        let id_c = CString::new(self.id.as_str()).unwrap_or_default();
        let title_cstrings: Vec<CString> = cols
            .iter()
            .map(|c| CString::new(c.title.as_str()).unwrap_or_default())
            .collect();

        let mut raw_cols: Vec<lens_sys::lens_datagrid_col> = cols
            .iter()
            .zip(&title_cstrings)
            .map(|(c, t)| lens_sys::lens_datagrid_col {
                title: t.as_ptr(),
                width: c.width,
                min_width: c.min_width,
                sort: c.sort.to_raw(),
                sortable: c.sortable,
                resizable: c.resizable,
                align: c.align.raw(),
            })
            .collect();

        let cell_cstrings: Vec<CString> = cell_texts
            .iter()
            .map(|&s| CString::new(s).unwrap_or_default())
            .collect();

        let cell_ptrs: Vec<*const std::os::raw::c_char> =
            cell_cstrings.iter().map(|cs| cs.as_ptr()).collect();

        let raw_opts = lens_sys::lens_datagrid_opts {
            height: self.height,
            header_height: self.header_height,
            row_height: self.row_height,
            striped: self.striped,
        };

        let mut raw_sel = *selected_row as u32;

        let act = unsafe {
            lens_sys::lens_datagrid(
                frame.as_raw(),
                id_c.as_ptr(),
                raw_cols.as_mut_ptr(),
                raw_cols.len() as u32,
                cell_ptrs.as_ptr(),
                row_count as u32,
                &mut raw_sel,
                &raw_opts,
            )
        };

        *selected_row = raw_sel as usize;

        // Copy back modified column widths and sort states
        for (c, raw) in cols.iter_mut().zip(&raw_cols) {
            c.width = raw.width;
            c.sort = SortDir::from_raw(raw.sort);
        }

        match act.kind {
            lens_sys::lens_datagrid_action_kind::LENS_DATAGRID_ACTION_ROW_CLICKED => {
                DataGridAction::RowClicked(act.row_index as usize)
            }
            lens_sys::lens_datagrid_action_kind::LENS_DATAGRID_ACTION_ROW_DOUBLE_CLICKED => {
                DataGridAction::RowDoubleClicked(act.row_index as usize)
            }
            lens_sys::lens_datagrid_action_kind::LENS_DATAGRID_ACTION_SORT_CHANGED => {
                DataGridAction::SortChanged {
                    col: act.col_index as usize,
                    dir: SortDir::from_raw(act.sort_dir),
                }
            }
            lens_sys::lens_datagrid_action_kind::LENS_DATAGRID_ACTION_COL_RESIZED => {
                DataGridAction::ColResized(act.col_index as usize)
            }
            _ => DataGridAction::None,
        }
    }
}
