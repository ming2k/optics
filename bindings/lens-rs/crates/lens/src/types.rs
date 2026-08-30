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

    pub fn raw(self) -> sys::flux_rect {
        self.to_raw()
    }
}

/// An sRGB colour with a premultiplied alpha channel, packed into a `u32` (0xAABBGGRR
/// little-endian). Mirrors `flux_color`.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct Color(pub sys::flux_color);

impl Color {
    pub const TRANSPARENT: Color = Color(0);

    pub fn rgb(r: u8, g: u8, b: u8) -> Color {
        Color::rgba(r, g, b, 255)
    }

    pub fn rgba(r: u8, g: u8, b: u8, a: u8) -> Color {
        // SAFETY: pure packing function, no state.
        Color(unsafe { sys::flux_color_rgba_premul(r, g, b, a) })
    }

    pub fn from_rgba(r: u8, g: u8, b: u8, a: u8) -> Color {
        Self::rgba(r, g, b, a)
    }

    pub fn with_alpha(self, a: u8) -> Color {
        let (r, g, b, _) = self.components();
        Color::rgba(r, g, b, a)
    }

    pub fn components(self) -> (u8, u8, u8, u8) {
        let r = ((self.0 >> 16) & 0xFF) as u8;
        let g = ((self.0 >> 8) & 0xFF) as u8;
        let b = (self.0 & 0xFF) as u8;
        let a = ((self.0 >> 24) & 0xFF) as u8;
        (r, g, b, a)
    }

    pub fn r(self) -> u8 {
        ((self.0 >> 16) & 0xFF) as u8
    }

    pub fn g(self) -> u8 {
        ((self.0 >> 8) & 0xFF) as u8
    }

    pub fn b(self) -> u8 {
        (self.0 & 0xFF) as u8
    }

    pub fn a(self) -> u8 {
        ((self.0 >> 24) & 0xFF) as u8
    }

    pub fn raw(self) -> sys::flux_color {
        self.0
    }

    pub fn from_raw(raw: sys::flux_color) -> Self {
        Self(raw)
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

/// Named vector glyphs bundled with the lens runtime.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Icon {
    Activity,
    BarChart,
    Bell,
    BookOpen,
    Briefcase,
    CheckCircle,
    Clock,
    Database,
    DollarSign,
    ExternalLink,
    Globe,
    Grid,
    Home,
    Link,
    MessageCircle,
    MousePointer,
    Radio,
    RefreshCw,
    Rss,
    Shield,
    Target,
    TrendingUp,
    Users,
    Zap,
    Settings,
    Sidebar,
    Sliders,
    Menu,
    FileText,
    FilePlus,
    Edit,
    Plus,
    Trash,
    ChevronLeft,
    ChevronRight,
    Repeat,
    Shuffle,
    SkipBack,
    Play,
    Pause,
    SkipForward,
    VolumeMuted,
    VolumeLow,
    VolumeHigh,
    Search,
    Star,
    StarRounded,
    StarRoundedFilled,
    X,
    Square,
    Circle,
    Slash,
    PenTool,
    Type,
    RotateCcw,
    RotateCw,
    ZoomIn,
    ZoomOut,
    Save,
    Folder,
    File,
    Bookmark,
    MoreHorizontal,
    ChevronDown,
    ChevronUp,
}

impl Icon {
    pub fn raw(self) -> sys::lens_icon_id {
        #[allow(clippy::wildcard_imports)]
        use sys::lens_icon_id as ids;
        match self {
            Icon::Activity => ids::LENS_ICON_ACTIVITY,
            Icon::BarChart => ids::LENS_ICON_BAR_CHART_2,
            Icon::Bell => ids::LENS_ICON_BELL,
            Icon::BookOpen => ids::LENS_ICON_BOOK_OPEN,
            Icon::Briefcase => ids::LENS_ICON_BRIEFCASE,
            Icon::CheckCircle => ids::LENS_ICON_CHECK_CIRCLE,
            Icon::Clock => ids::LENS_ICON_CLOCK,
            Icon::Database => ids::LENS_ICON_DATABASE,
            Icon::DollarSign => ids::LENS_ICON_DOLLAR_SIGN,
            Icon::ExternalLink => ids::LENS_ICON_EXTERNAL_LINK,
            Icon::Globe => ids::LENS_ICON_GLOBE,
            Icon::Grid => ids::LENS_ICON_GRID,
            Icon::Home => ids::LENS_ICON_HOME,
            Icon::Link => ids::LENS_ICON_LINK,
            Icon::MessageCircle => ids::LENS_ICON_MESSAGE_CIRCLE,
            Icon::MousePointer => ids::LENS_ICON_MOUSE_POINTER,
            Icon::Radio => ids::LENS_ICON_RADIO,
            Icon::RefreshCw => ids::LENS_ICON_REFRESH_CW,
            Icon::Rss => ids::LENS_ICON_RSS,
            Icon::Shield => ids::LENS_ICON_SHIELD,
            Icon::Target => ids::LENS_ICON_TARGET,
            Icon::TrendingUp => ids::LENS_ICON_TRENDING_UP,
            Icon::Users => ids::LENS_ICON_USERS,
            Icon::Zap => ids::LENS_ICON_ZAP,
            Icon::Settings => ids::LENS_ICON_SETTINGS,
            Icon::Sidebar => ids::LENS_ICON_SIDEBAR,
            Icon::Sliders => ids::LENS_ICON_SLIDERS,
            Icon::Menu => ids::LENS_ICON_MENU,
            Icon::FileText => ids::LENS_ICON_FILE_TEXT,
            Icon::FilePlus => ids::LENS_ICON_FILE_PLUS,
            Icon::Edit => ids::LENS_ICON_EDIT,
            Icon::Plus => ids::LENS_ICON_PLUS,
            Icon::Trash => ids::LENS_ICON_TRASH_2,
            Icon::ChevronLeft => ids::LENS_ICON_CHEVRON_LEFT,
            Icon::ChevronRight => ids::LENS_ICON_CHEVRON_RIGHT,
            Icon::Repeat => ids::LENS_ICON_REPEAT,
            Icon::Shuffle => ids::LENS_ICON_SHUFFLE,
            Icon::SkipBack => ids::LENS_ICON_SKIP_BACK,
            Icon::Play => ids::LENS_ICON_PLAY,
            Icon::Pause => ids::LENS_ICON_PAUSE,
            Icon::SkipForward => ids::LENS_ICON_SKIP_FORWARD,
            Icon::VolumeMuted => ids::LENS_ICON_VOLUME_X,
            Icon::VolumeLow => ids::LENS_ICON_VOLUME_1,
            Icon::VolumeHigh => ids::LENS_ICON_VOLUME_2,
            Icon::Search => ids::LENS_ICON_SEARCH,
            Icon::Star => ids::LENS_ICON_STAR,
            Icon::StarRounded => ids::LENS_ICON_STAR_ROUNDED,
            Icon::StarRoundedFilled => ids::LENS_ICON_STAR_ROUNDED_FILLED,
            Icon::X => ids::LENS_ICON_X,
            Icon::Square => ids::LENS_ICON_SQUARE,
            Icon::Circle => ids::LENS_ICON_CIRCLE,
            Icon::Slash => ids::LENS_ICON_SLASH,
            Icon::PenTool => ids::LENS_ICON_PEN_TOOL,
            Icon::Type => ids::LENS_ICON_TYPE,
            Icon::RotateCcw => ids::LENS_ICON_ROTATE_CCW,
            Icon::RotateCw => ids::LENS_ICON_ROTATE_CW,
            Icon::ZoomIn => ids::LENS_ICON_ZOOM_IN,
            Icon::ZoomOut => ids::LENS_ICON_ZOOM_OUT,
            Icon::Save => ids::LENS_ICON_SAVE,
            Icon::Folder => ids::LENS_ICON_FOLDER,
            Icon::File => ids::LENS_ICON_FILE,
            Icon::Bookmark => ids::LENS_ICON_BOOKMARK,
            Icon::MoreHorizontal => ids::LENS_ICON_MORE_HORIZONTAL,
            Icon::ChevronDown => ids::LENS_ICON_CHEVRON_DOWN,
            Icon::ChevronUp => ids::LENS_ICON_CHEVRON_UP,
        }
    }
}

impl From<Icon> for sys::lens_icon_id {
    fn from(icon: Icon) -> Self {
        icon.raw()
    }
}

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
    #[allow(dead_code)]
    pub fn raw(self) -> sys::lens_button_variant {
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
    #[allow(dead_code)]
    pub fn raw(self) -> sys::lens_checkbox_appearance {
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
    Start,
    Center,
    End,
    #[default]
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

    pub fn default() -> Theme {
        Theme(unsafe { sys::lens_theme_default() })
    }

    pub fn dark() -> Theme {
        Theme(unsafe { sys::lens_theme_dark() })
    }

    pub fn from_raw(raw: sys::lens_theme) -> Theme {
        Theme(raw)
    }

    pub fn raw(&self) -> sys::lens_theme {
        self.0
    }

    pub fn with_bg(mut self, color: Color) -> Theme {
        self.0.color_bg = color.raw();
        self
    }

    pub fn with_fg(mut self, color: Color) -> Theme {
        self.0.color_fg = color.raw();
        self
    }

    pub fn with_accent(mut self, color: Color) -> Theme {
        self.0.color_accent = color.raw();
        self
    }

    pub fn with_border(mut self, color: Color) -> Theme {
        self.0.color_border = color.raw();
        self
    }

    pub fn with_hover(mut self, color: Color) -> Theme {
        self.0.color_hover = color.raw();
        self
    }

    pub fn with_active(mut self, color: Color) -> Theme {
        self.0.color_active = color.raw();
        self
    }

    pub fn with_disabled(mut self, color: Color) -> Theme {
        self.0.color_disabled = color.raw();
        self
    }

    pub fn with_error(mut self, color: Color) -> Theme {
        self.0.color_error = color.raw();
        self
    }

    pub fn with_slider_track_color(mut self, color: Color) -> Theme {
        self.0.color_slider_track = color.raw();
        self
    }

    pub fn with_slider_fill_color(mut self, color: Color) -> Theme {
        self.0.color_slider_fill = color.raw();
        self
    }

    pub fn with_slider_knob_color(mut self, color: Color) -> Theme {
        self.0.color_slider_knob = color.raw();
        self
    }

    pub fn with_slider_track_thickness(mut self, thickness: f32) -> Theme {
        self.0.slider_track_thickness = thickness;
        self
    }

    pub fn with_slider_knob_size(mut self, size: f32) -> Theme {
        self.0.slider_knob_size = size;
        self
    }

    pub fn with_scrollbar_track_color(mut self, color: Color) -> Theme {
        self.0.color_scrollbar_track = color.raw();
        self
    }

    pub fn with_scrollbar_thumb_color(mut self, color: Color) -> Theme {
        self.0.color_scrollbar_thumb = color.raw();
        self
    }

    pub fn with_scrollbar_thumb_hover_color(mut self, color: Color) -> Theme {
        self.0.color_scrollbar_thumb_hover = color.raw();
        self
    }

    pub fn with_scrollbar_thumb_active_color(mut self, color: Color) -> Theme {
        self.0.color_scrollbar_thumb_active = color.raw();
        self
    }

    pub fn with_scrollbar_width(mut self, width: f32) -> Theme {
        self.0.scrollbar_width = width;
        self
    }

    pub fn with_scrollbar_radius(mut self, radius: f32) -> Theme {
        self.0.scrollbar_radius = radius;
        self
    }

    pub fn with_scrollbar_min_thumb_h(mut self, min_h: f32) -> Theme {
        self.0.scrollbar_min_thumb_h = min_h;
        self
    }

    pub fn with_font_size(mut self, size: f32) -> Theme {
        self.0.font_size = size;
        self
    }

    pub fn with_font_size_sm(mut self, size: f32) -> Theme {
        self.0.font_size_h3 = size;
        self
    }

    pub fn with_font_size_lg(mut self, size: f32) -> Theme {
        self.0.font_size_title = size;
        self
    }

    pub fn with_scrollbar_pad(self, _pad: f32) -> Theme {
        self
    }

    pub fn font_size_sm(&self) -> f32 {
        self.0.font_size_h3
    }

    pub fn font_size_lg(&self) -> f32 {
        self.0.font_size_title
    }

    pub fn scrollbar_pad(&self) -> f32 {
        0.0
    }

    pub fn with_padding(mut self, padding: f32) -> Theme {
        self.0.padding = padding;
        self
    }

    pub fn with_pad(mut self, pad: f32) -> Theme {
        self.0.padding = pad;
        self
    }

    pub fn with_gap(mut self, gap: f32) -> Theme {
        self.0.gap = gap;
        self
    }

    pub fn with_corner_radius(mut self, radius: f32) -> Theme {
        self.0.corner_radius = radius;
        self
    }

    pub fn with_border_width(mut self, width: f32) -> Theme {
        self.0.border_width = width;
        self
    }

    pub fn bg(&self) -> Color {
        Color::from_raw(self.0.color_bg)
    }

    pub fn fg(&self) -> Color {
        Color::from_raw(self.0.color_fg)
    }

    pub fn accent(&self) -> Color {
        Color::from_raw(self.0.color_accent)
    }

    pub fn border(&self) -> Color {
        Color::from_raw(self.0.color_border)
    }

    pub fn hover(&self) -> Color {
        Color::from_raw(self.0.color_hover)
    }

    pub fn active(&self) -> Color {
        Color::from_raw(self.0.color_active)
    }

    pub fn disabled(&self) -> Color {
        Color::from_raw(self.0.color_disabled)
    }

    pub fn error(&self) -> Color {
        Color::from_raw(self.0.color_error)
    }

    pub fn font_size(&self) -> f32 {
        self.0.font_size
    }

    pub fn corner_radius(&self) -> f32 {
        self.0.corner_radius
    }

    pub fn border_width(&self) -> f32 {
        self.0.border_width
    }

    pub fn padding(&self) -> f32 {
        self.0.padding
    }

    pub fn pad(&self) -> f32 {
        self.0.padding
    }

    pub fn gap(&self) -> f32 {
        self.0.gap
    }

    pub fn scrollbar_width(&self) -> f32 {
        self.0.scrollbar_width
    }

    pub fn scrollbar_radius(&self) -> f32 {
        self.0.scrollbar_radius
    }

    pub fn scrollbar_min_thumb_h(&self) -> f32 {
        self.0.scrollbar_min_thumb_h
    }

    pub fn slider_track_thickness(&self) -> f32 {
        self.0.slider_track_thickness
    }

    pub fn slider_knob_size(&self) -> f32 {
        self.0.slider_knob_size
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

    pub fn with_bg_hover(mut self, color: Color) -> Style {
        self.0.bg_hover = color.raw();
        self.0.fields |= sys::lens_style_field::LENS_STYLE_BG_HOVER as u32;
        self
    }

    pub fn with_bg_pressed(mut self, color: Color) -> Style {
        self.0.bg_pressed = color.raw();
        self.0.fields |= sys::lens_style_field::LENS_STYLE_BG_PRESSED as u32;
        self
    }

    pub fn with_border_width(mut self, width: f32) -> Style {
        self.0.border_width = width;
        self.0.fields |= sys::lens_style_field::LENS_STYLE_BORDER_WIDTH as u32;
        self
    }

    pub fn with_padding(mut self, padding: f32) -> Style {
        self.0.padding = padding;
        self.0.fields |= sys::lens_style_field::LENS_STYLE_PADDING as u32;
        self
    }

    pub fn with_pad(mut self, pad: f32) -> Style {
        self.0.padding = pad;
        self.0.fields |= sys::lens_style_field::LENS_STYLE_PADDING as u32;
        self
    }

    pub fn with_gap(mut self, gap: f32) -> Style {
        self.0.gap = gap;
        self.0.fields |= sys::lens_style_field::LENS_STYLE_GAP as u32;
        self
    }

    pub fn with_outline_color(mut self, color: Color) -> Style {
        self.0.outline_color = color.raw();
        self.0.fields |= sys::lens_style_field::LENS_STYLE_OUTLINE_COLOR as u32;
        self
    }

    pub fn with_outline_width(mut self, width: f32) -> Style {
        self.0.outline_width = width;
        self.0.fields |= sys::lens_style_field::LENS_STYLE_OUTLINE_WIDTH as u32;
        self
    }

    pub fn raw(self) -> sys::lens_style {
        self.0
    }

    pub fn from_raw(raw: sys::lens_style) -> Style {
        Style(raw)
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
        let mut b: sys::lens_box = unsafe { std::mem::zeroed() };
        b.flex = self.flex;
        b.width = self.width;
        b.height = self.height;
        b.min_width = self.min_width;
        b.max_width = self.max_width;
        b.min_height = self.min_height;
        b.max_height = self.max_height;
        sys::lens_layout_opts {
            box_: b,
            gap: self.gap,
            pad: self.pad,
            align: sys::lens_align::LENS_START,
            cross: self.cross.raw(),
            bg: self.bg.raw(),
            radius: self.radius,
            border: self.border.raw(),
            border_width: self.border_width,
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
            box_: unsafe { std::mem::zeroed() },
        }
    }
}
