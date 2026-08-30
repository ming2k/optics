//! Small value types shared across the safe surface: geometry, colour, the
//! interaction response, and placement options.

use lens_sys as sys;

/// An axis-aligned rectangle in UI-space (logical) pixels. Mirrors `flux_rect`.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Rect {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
}

impl Rect {
    pub const ZERO: Rect = Rect {
        x: 0.0,
        y: 0.0,
        w: 0.0,
        h: 0.0,
    };

    pub(crate) fn from_raw(r: sys::flux_rect) -> Rect {
        Rect {
            x: r.x,
            y: r.y,
            w: r.w,
            h: r.h,
        }
    }

    pub(crate) fn to_raw(self) -> sys::flux_rect {
        sys::flux_rect {
            x: self.x,
            y: self.y,
            w: self.w,
            h: self.h,
        }
    }
}

/// An sRGB colour with a premultiplied alpha channel, packed into a `u32` (0xAABBGGRR
/// little-endian). Mirrors `flux_color`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Color(pub(crate) sys::flux_color);

impl Color {
    pub const TRANSPARENT: Color = Color(0);

    pub fn rgb(r: u8, g: u8, b: u8) -> Color {
        Color::rgba(r, g, b, 255)
    }

    pub fn rgba(r: u8, g: u8, b: u8, a: u8) -> Color {
        // SAFETY: pure packing function, no state.
        Color(unsafe { sys::flux_color_rgba_premul(r, g, b, a) })
    }

    pub(crate) fn raw(self) -> sys::flux_color {
        self.0
    }
}

/// Widget-kind tags for skin replacement ([`crate::Frame::set_skin`],
/// ADR-0059). Re-exported from the raw bindings.
pub use sys::lens_widget_kind as WidgetKind;

/// The per-kind content payload of a [`WidgetRecord`]. Raw bindgen layout.
pub use sys::lens_widget_content as WidgetContent;

/// One wrapped/visible text line in a LABEL or TEXTEDIT record payload.
pub use sys::lens_text_line as TextLine;

/// The semantic cursor hint returned by [`Ui::cursor_hint`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(u32)]
pub enum CursorHint {
    #[default]
    Default = sys::lens_cursor_hint::LENS_CURSOR_DEFAULT as u32,
    Pointer = sys::lens_cursor_hint::LENS_CURSOR_POINTER as u32,
    Text = sys::lens_cursor_hint::LENS_CURSOR_TEXT as u32,
    ResizeNs = sys::lens_cursor_hint::LENS_CURSOR_RESIZE_NS as u32,
    ResizeEw = sys::lens_cursor_hint::LENS_CURSOR_RESIZE_EW as u32,
}

impl CursorHint {
    pub(crate) fn from_raw(raw: sys::lens_cursor_hint) -> Self {
        match raw {
            sys::lens_cursor_hint::LENS_CURSOR_POINTER => Self::Pointer,
            sys::lens_cursor_hint::LENS_CURSOR_TEXT => Self::Text,
            sys::lens_cursor_hint::LENS_CURSOR_RESIZE_NS => Self::ResizeNs,
            sys::lens_cursor_hint::LENS_CURSOR_RESIZE_EW => Self::ResizeEw,
            _ => Self::Default,
        }
    }
}
pub use sys::lens_style_resolved as StyleResolved;
pub use sys::lens_widget_record as WidgetRecord;
pub use sys::lens_widget_state as WidgetState;
pub use sys::lens_skin_fn as SkinFn;
pub use sys::lens_icon_id as Icon;

/// Font family selection.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum FontFamily {
    #[default]
    Sans,
    Serif,
    Mono,
}

impl FontFamily {
    pub(crate) fn raw(self) -> sys::lens_text_family {
        match self {
            FontFamily::Sans => sys::lens_text_family::LENS_TEXT_FAMILY_SANS,
            FontFamily::Serif => sys::lens_text_family::LENS_TEXT_FAMILY_SERIF,
            FontFamily::Mono => sys::lens_text_family::LENS_TEXT_FAMILY_MONO,
        }
    }
}

/// Text metrics.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct TextMetrics {
    pub width: f32,
    pub height: f32,
    pub baseline: f32,
}

impl TextMetrics {
    pub(crate) fn from_raw(m: sys::lens_text_metrics) -> Self {
        Self {
            width: m.width,
            height: m.height,
            baseline: m.baseline,
        }
    }
}

/// Button styling variant.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ButtonVariant {
    #[default]
    Default,
    Primary,
    Subtle,
    Link,
}

impl ButtonVariant {
    pub(crate) fn raw(self) -> sys::lens_button_variant {
        match self {
            ButtonVariant::Default => sys::lens_button_variant::LENS_BUTTON_DEFAULT,
            ButtonVariant::Primary => sys::lens_button_variant::LENS_BUTTON_PRIMARY,
            ButtonVariant::Subtle => sys::lens_button_variant::LENS_BUTTON_SUBTLE,
            ButtonVariant::Link => sys::lens_button_variant::LENS_BUTTON_LINK,
        }
    }
}

/// Checkbox appearance styling.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum CheckboxAppearance {
    #[default]
    Box,
    Switch,
    Radio,
}

impl CheckboxAppearance {
    pub(crate) fn raw(self) -> sys::lens_checkbox_appearance {
        match self {
            CheckboxAppearance::Box => sys::lens_checkbox_appearance::LENS_CHECKBOX_BOX,
            CheckboxAppearance::Switch => sys::lens_checkbox_appearance::LENS_CHECKBOX_SWITCH,
            CheckboxAppearance::Radio => sys::lens_checkbox_appearance::LENS_CHECKBOX_RADIO,
        }
    }
}

/// Flex alignment along the cross axis (ADR-0028).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Align {
    #[default]
    Start,
    Center,
    End,
    Stretch,
}

impl Align {
    pub(crate) fn raw(self) -> sys::lens_align {
        match self {
            Align::Start => sys::lens_align::LENS_START,
            Align::Center => sys::lens_align::LENS_CENTER,
            Align::End => sys::lens_align::LENS_END,
            Align::Stretch => sys::lens_align::LENS_STRETCH,
        }
    }
}

/// The interaction result of the most recently built widget.
#[derive(Debug, Clone, Copy, Default)]
pub struct Response {
    pub rect: Rect,
    pub hovered: bool,
    pub pressed: bool,
    pub clicked: bool,
    pub right_clicked: bool,
    pub focused: bool,
    pub focus_visible: bool,
    pub changed: bool,
    pub id: u64,
    pub state: u32,
}

impl Response {
    pub(crate) fn from_raw(r: sys::lens_response) -> Response {
        Response {
            rect: Rect::from_raw(r.rect),
            hovered: r.hovered,
            pressed: r.pressed,
            clicked: r.clicked,
            right_clicked: r.right_clicked,
            focused: r.focused,
            focus_visible: (r.state & (sys::lens_widget_state::LENS_STATE_FOCUS_VISIBLE as u32)) != 0,
            changed: r.changed,
            id: r.id,
            state: r.state,
        }
    }
}

/// Visual theme token dictionary (ADR-0032).
#[derive(Debug, Clone, Copy)]
pub struct Theme(pub(crate) sys::lens_theme);

impl Theme {
    pub fn light() -> Theme {
        Theme(unsafe { sys::lens_theme_default() })
    }

    pub fn dark() -> Theme {
        Theme(unsafe { sys::lens_theme_dark() })
    }

    pub fn with_corner_radius(mut self, radius: f32) -> Theme {
        self.0.corner_radius = radius;
        self
    }

    pub fn with_accent(mut self, color: Color) -> Theme {
        self.0.color_accent = color.raw();
        self
    }
}

impl Default for Theme {
    fn default() -> Self {
        Theme::dark()
    }
}

/// A set of style overrides.
#[derive(Debug, Clone, Copy, Default)]
pub struct Style(pub(crate) sys::lens_style);

impl Style {
    pub fn new() -> Style {
        Style::default()
    }

    pub fn with_bg(mut self, color: Color) -> Style {
        self.0.bg = color.raw();
        self.0.fields |= sys::lens_style_field::LENS_STYLE_BG as u32;
        self
    }

    pub fn with_fg(mut self, color: Color) -> Style {
        self.0.fg = color.raw();
        self.0.fields |= sys::lens_style_field::LENS_STYLE_FG as u32;
        self
    }

    pub fn with_accent(mut self, color: Color) -> Style {
        self.0.accent = color.raw();
        self.0.fields |= sys::lens_style_field::LENS_STYLE_ACCENT as u32;
        self
    }

    pub fn with_border(mut self, color: Color) -> Style {
        self.0.border = color.raw();
        self.0.fields |= sys::lens_style_field::LENS_STYLE_BORDER as u32;
        self
    }

    pub fn with_corner_radius(mut self, r: f32) -> Style {
        self.0.corner_radius = r;
        self.0.fields |= sys::lens_style_field::LENS_STYLE_CORNER_RADIUS as u32;
        self
    }

    pub fn with_font_size(mut self, size: f32) -> Style {
        self.0.font_size = size;
        self.0.fields |= sys::lens_style_field::LENS_STYLE_FONT_SIZE as u32;
        self
    }
}

/// A closed z band for an absolutely-placed subtree ([`crate::Frame::place`], ADR-0060).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Band {
    Backdrop,
    Base,
    Chrome,
    #[default]
    Popup,
    Modal,
    Tooltip,
    Notify,
}

impl Band {
    pub(crate) fn raw(self) -> sys::lens_band {
        match self {
            Band::Backdrop => sys::lens_band::LENS_BAND_BACKDROP,
            Band::Base => sys::lens_band::LENS_BAND_BASE,
            Band::Chrome => sys::lens_band::LENS_BAND_CHROME,
            Band::Popup => sys::lens_band::LENS_BAND_POPUP,
            Band::Modal => sys::lens_band::LENS_BAND_MODAL,
            Band::Tooltip => sys::lens_band::LENS_BAND_TOOLTIP,
            Band::Notify => sys::lens_band::LENS_BAND_NOTIFY,
        }
    }
}

/// Placement mode for `lens_place`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum PlaceMode {
    Exact,
    #[default]
    Anchored,
    Centered,
    Tooltip,
    Backdrop,
}

impl PlaceMode {
    pub(crate) fn raw(self) -> sys::lens_place_mode {
        match self {
            PlaceMode::Exact => sys::lens_place_mode::LENS_PLACE_EXACT,
            PlaceMode::Anchored => sys::lens_place_mode::LENS_PLACE_ANCHORED,
            PlaceMode::Centered => sys::lens_place_mode::LENS_PLACE_CENTERED,
            PlaceMode::Tooltip => sys::lens_place_mode::LENS_PLACE_TOOLTIP,
            PlaceMode::Backdrop => sys::lens_place_mode::LENS_PLACE_BACKDROP,
        }
    }
}

/// General layout configuration for flex containers.
#[derive(Debug, Clone, Copy, Default)]
pub struct LayoutOpts {
    pub flex: f32,
    pub width: f32,
    pub height: f32,
    pub gap: f32,
    pub pad: f32,
    pub cross: Align,
    pub bg: Color,
    pub radius: f32,
    pub border: Color,
    pub border_width: f32,
    pub min_width: f32,
    pub max_width: f32,
    pub min_height: f32,
    pub max_height: f32,
}

impl LayoutOpts {
    pub fn new() -> Self {
        Self::default()
    }

    pub(crate) fn to_raw(self) -> sys::lens_layout_opts {
        sys::lens_layout_opts {
            gap: self.gap,
            pad: self.pad,
            cross: self.cross.raw(),
            bg: self.bg.raw(),
            radius: self.radius,
            border: self.border.raw(),
            border_width: self.border_width,
            min_width: self.min_width,
            max_width: self.max_width,
            min_height: self.min_height,
            max_height: self.max_height,
            box_: sys::lens_box {
                flex: self.flex,
                width: self.width,
                height: self.height,
                ..Default::default()
            },
            align: sys::lens_align::LENS_START,
        }
    }
}

/// Placement options for absolute overlays.
#[derive(Debug, Clone, Copy, Default)]
pub struct PlaceOpts {
    pub band: Band,
    pub mode: PlaceMode,
    pub rect: Rect,
    pub bounds: Rect,
    pub transient: bool,
    pub interactive: bool,
    pub layout: LayoutOpts,
}

impl PlaceOpts {
    pub(crate) fn to_raw(self) -> sys::lens_place_opts {
        sys::lens_place_opts {
            band: self.band.raw(),
            mode: self.mode.raw(),
            rect: self.rect.to_raw(),
            bounds: self.bounds.to_raw(),
            transient: self.transient,
            interactive: self.interactive,
            layout: self.layout.to_raw(),
            box_: sys::lens_box::default(),
        }
    }
}
