//! Higher-level interaction patterns and compound protocols.

pub mod segmented;
pub mod split;
pub mod tabs;
pub mod virtual_grid;

pub use segmented::{SegmentedControl, SegmentedItem};
pub use split::{SplitOpts, split_handle_v};
pub use tabs::{TabAction, TabItem, TabStrip};
pub use virtual_grid::{VirtualGridPlan, virtual_grid_calc};
