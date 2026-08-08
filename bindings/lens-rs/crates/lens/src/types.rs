//! Small value types shared across the safe surface: geometry, colour, the
//! interaction response, and overlay options.

use lens_sys as sys;

/// An axis-aligned rectangle in UI-space (logical) pixels. Mirrors `flux_rect`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Rect {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
}

impl Rect {
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

/// Shaped text extent in logical pixels.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TextMetrics {
    pub width: f32,
    pub height: f32,
    /// Baseline offset measured from the top of the text run.
    pub baseline: f32,
}

impl TextMetrics {
    pub(crate) fn from_raw(metrics: sys::lens_text_metrics) -> Self {
        Self {
            width: metrics.width,
            height: metrics.height,
            baseline: metrics.baseline,
        }
    }
}

/// A packed RGBA colour (`flux_color`). Construct with [`Color::rgba`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Color(pub sys::flux_color);

impl Color {
    /// Fully transparent (alpha 0). In overlay options this means "no fill".
    pub const TRANSPARENT: Color = Color(0);

    /// A colour from straight-alpha 8-bit components, stored in flux's
    /// premultiplied representation for correct SRC_OVER blending.
    pub fn rgba(r: u8, g: u8, b: u8, a: u8) -> Color {
        // SAFETY: pure packing function, no state.
        Color(unsafe { sys::flux_color_rgba_premul(r, g, b, a) })
    }

    /// Return the 8-bit RGBA components in linear-space byte form
    /// (the same packing order flux uses internally).
    pub fn components(self) -> (u8, u8, u8, u8) {
        let c = self.0;
        (
            ((c >> 16) & 0xff) as u8,
            ((c >> 8) & 0xff) as u8,
            (c & 0xff) as u8,
            ((c >> 24) & 0xff) as u8,
        )
    }

    /// Return a new colour with the same RGB and the given alpha.
    /// The result is stored premultiplied, matching flux's internal format.
    pub fn with_alpha(self, a: u8) -> Color {
        let (pr, pg, pb, old_a) = self.components();
        if old_a == 0 || a == 0 {
            return Color::TRANSPARENT;
        }
        let unpremultiply = |component: u8| {
            let value = (u32::from(component) * 255 + u32::from(old_a) / 2) / u32::from(old_a);
            u8::try_from(value.min(255)).unwrap_or(255)
        };
        let r = unpremultiply(pr);
        let g = unpremultiply(pg);
        let b = unpremultiply(pb);
        // SAFETY: pure packing function, no state.
        Color(unsafe { sys::flux_color_rgba_premul(r, g, b, a) })
    }

    pub(crate) fn raw(self) -> sys::flux_color {
        self.0
    }
}

/// A restrained contour drawn behind text, vector icons, or alpha-backed
/// images that float over variable imagery or translucent material.
///
/// The contour is deliberately opt-in. Use it to preserve foreground
/// separation where the background cannot be predicted, not as a global
/// decoration for ordinary content.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ForegroundOutline {
    pub color: Color,
    /// Outward visual radius in logical pixels.
    pub width: f32,
}

impl ForegroundOutline {
    pub fn new(color: Color, width: f32) -> Self {
        Self {
            color,
            width: width.max(0.0),
        }
    }

    pub(crate) fn raw(self) -> sys::lens_foreground_outline {
        sys::lens_foreground_outline {
            color: self.color.raw(),
            width: self.width.max(0.0),
        }
    }
}

impl Default for ForegroundOutline {
    fn default() -> Self {
        Self {
            color: Color::TRANSPARENT,
            width: 0.0,
        }
    }
}

/// Cross-axis alignment, mirroring `lens_align`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Align {
    Start,
    Center,
    End,
    Stretch,
}

/// One column in a virtualized [`crate::Frame::table`]. A zero width shares
/// the remaining table width with the other flexible columns.
#[derive(Debug, Clone, Copy)]
pub struct TableColumn<'a> {
    pub title: &'a str,
    pub width: f32,
    pub align: Align,
}

/// Presentation and interaction options for a virtualized table.
#[derive(Debug, Clone, Copy)]
pub struct TableOpts {
    pub row_height: f32,
    pub show_header: bool,
    pub selectable: bool,
    pub zebra: bool,
}

/// Visual relationship between tabs in a [`crate::Frame::tabs_ex`] strip.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum TabStyle {
    /// Compact independent tabs with the standard Lens active indicator.
    #[default]
    Standard,
    /// Tabs share a rail; the active surface joins adjacent tabs with curved
    /// connectors and follows the theme's active/accent colours.
    Connected,
    /// Compact tabs with a theme-coloured indicator whose independently
    /// sprung edges stretch and settle when selection changes.
    Indicator,
}

/// Presentation options for a tab strip. Zero/transparent values inherit the
/// current theme, so selecting a style does not require duplicating tokens.
#[derive(Debug, Clone, Copy)]
pub struct TabsOpts {
    pub style: TabStyle,
    pub rail_color: Color,
    pub active_color: Color,
    pub hover_color: Color,
    pub indicator_color: Color,
    pub radius: f32,
    pub connector_size: f32,
    pub indicator_thickness: f32,
    pub indicator_gap: f32,
    pub indicator_padding: f32,
    /// Give every tab the same share of the strip's available width.
    /// This layout policy is independent of [`TabStyle`].
    pub equal_width: bool,
}

impl Default for TabsOpts {
    fn default() -> Self {
        Self {
            style: TabStyle::Standard,
            rail_color: Color::TRANSPARENT,
            active_color: Color::TRANSPARENT,
            hover_color: Color::TRANSPARENT,
            indicator_color: Color::TRANSPARENT,
            radius: 0.0,
            connector_size: 0.0,
            indicator_thickness: 0.0,
            indicator_gap: 0.0,
            indicator_padding: 0.0,
            equal_width: false,
        }
    }
}

impl TabsOpts {
    pub(crate) fn to_raw(self) -> sys::lens_tabs_opts {
        sys::lens_tabs_opts {
            style: match self.style {
                TabStyle::Standard => sys::lens_tab_style::LENS_TAB_STYLE_STANDARD,
                TabStyle::Connected => sys::lens_tab_style::LENS_TAB_STYLE_CONNECTED,
                TabStyle::Indicator => sys::lens_tab_style::LENS_TAB_STYLE_INDICATOR,
            },
            rail_color: self.rail_color.raw(),
            active_color: self.active_color.raw(),
            hover_color: self.hover_color.raw(),
            indicator_color: self.indicator_color.raw(),
            radius: self.radius.max(0.0),
            connector_size: self.connector_size.max(0.0),
            indicator_thickness: self.indicator_thickness.max(0.0),
            indicator_gap: self.indicator_gap.max(0.0),
            indicator_padding: self.indicator_padding.max(0.0),
            equal_width: self.equal_width,
        }
    }
}

impl Default for TableOpts {
    fn default() -> Self {
        Self {
            row_height: 0.0,
            show_header: true,
            selectable: true,
            zebra: false,
        }
    }
}

/// Interaction result returned by [`crate::Frame::table`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TableResult {
    pub selected: Option<usize>,
    pub selection_changed: bool,
    pub clicked: bool,
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

/// The interaction result of the most recently built widget
/// ([`crate::Frame::response`]). Mirrors `lens_response`.
#[derive(Debug, Clone, Copy)]
pub struct Response {
    /// Last frame's final rect (what was hit-tested) — the natural anchor for
    /// an [`crate::OverlayOpts`] popup.
    pub rect: Rect,
    pub hovered: bool,
    pub pressed: bool,
    pub clicked: bool,
    pub right_clicked: bool,
    pub middle_clicked: bool,
    pub changed: bool,
    pub focused: bool,
}

impl Response {
    pub(crate) fn from_raw(r: sys::lens_response) -> Response {
        Response {
            rect: Rect::from_raw(r.rect),
            hovered: r.hovered,
            pressed: r.pressed,
            clicked: r.clicked,
            right_clicked: r.right_clicked,
            middle_clicked: r.middle_clicked,
            changed: r.changed,
            focused: r.focused,
        }
    }
}

/// Semantic cursor requested by the hovered Lens widget. Windowing hosts map
/// this hint to their platform cursor once after building each frame.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum CursorHint {
    #[default]
    Default,
    Pointer,
    Text,
    ResizeEw,
    ResizeNs,
}

impl CursorHint {
    pub(crate) fn from_raw(hint: sys::lens_cursor_hint) -> Self {
        use sys::lens_cursor_hint::*;
        match hint {
            LENS_CURSOR_POINTER => Self::Pointer,
            LENS_CURSOR_TEXT => Self::Text,
            LENS_CURSOR_RESIZE_EW => Self::ResizeEw,
            LENS_CURSOR_RESIZE_NS => Self::ResizeNs,
            LENS_CURSOR_DEFAULT => Self::Default,
        }
    }
}

/// Typeface family for subsequently built widgets ([`crate::Frame::set_text_family`]).
///
/// Like the theme setters, this is a context switch read at widget-build
/// time: set it, build the widgets that need another voice (a serif display
/// title, a monospace readout), then set it back. `Default` keeps the
/// engine's sans-serif default.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum FontFamily {
    #[default]
    Default,
    Sans,
    Serif,
    Mono,
}

impl FontFamily {
    pub(crate) fn raw(self) -> sys::lens_text_family {
        match self {
            FontFamily::Default => sys::lens_text_family::LENS_TEXT_FAMILY_DEFAULT,
            FontFamily::Sans => sys::lens_text_family::LENS_TEXT_FAMILY_SANS,
            FontFamily::Serif => sys::lens_text_family::LENS_TEXT_FAMILY_SERIF,
            FontFamily::Mono => sys::lens_text_family::LENS_TEXT_FAMILY_MONO,
        }
    }
}

/// A token set (colours, sizes, radii) that drives lens's appearance.
/// Construct a built-in set, adjust tokens with the `with_*` methods, and pass
/// it to [`crate::Ui::set_theme`].
#[derive(Clone, Copy)]
pub struct Theme(pub(crate) sys::lens_theme);

impl Theme {
    /// The default (light) token set.
    pub fn light() -> Theme {
        // SAFETY: pure constructor, no state.
        Theme(unsafe { sys::lens_theme_default() })
    }

    /// The dark token set.
    pub fn dark() -> Theme {
        // SAFETY: pure constructor, no state.
        Theme(unsafe { sys::lens_theme_dark() })
    }

    /// Set the shared corner radius used by rounded controls.
    pub fn with_corner_radius(mut self, radius: f32) -> Theme {
        self.0.corner_radius = radius.max(0.0);
        self
    }

    /// Set the shared border stroke width used by bordered controls.
    pub fn with_border_width(mut self, width: f32) -> Theme {
        self.0.border_width = width.max(0.0);
        self
    }

    /// Set the optional active-indicator width used by selectable rows and
    /// ghost icon buttons ([`crate::Frame::icon_button_active`]). When > 0,
    /// the active state draws a flush left accent rail of this width and tints
    /// the glyph or text with the accent colour. The default is 0: no rail,
    /// with the background tint as the plain active treatment.
    pub fn with_active_indicator_width(mut self, width: f32) -> Theme {
        self.0.active_indicator_width = width.max(0.0);
        self
    }

    /// Set the scrollbar visual width for scroll areas, in logical pixels.
    /// The same amount is reserved on the content's cross axis when a
    /// scroll area overflows, so nothing paints underneath the thumb.
    pub fn with_scrollbar_width(mut self, width: f32) -> Theme {
        self.0.scrollbar_width = width.max(0.0);
        self
    }

    /// Set the corner radius of the scrollbar thumb and track. Use
    /// `width * 0.5` for a fully rounded pill.
    pub fn with_scrollbar_radius(mut self, radius: f32) -> Theme {
        self.0.scrollbar_radius = radius.max(0.0);
        self
    }

    /// Set the minimum scrollbar thumb height. Keeps very long lists
    /// grabbable by preventing the thumb from shrinking to a few pixels.
    pub fn with_scrollbar_min_thumb_h(mut self, min_h: f32) -> Theme {
        self.0.scrollbar_min_thumb_h = min_h.max(0.0);
        self
    }

    /// Set the scrollbar track fill colour. A transparent colour (alpha 0)
    /// hides the track entirely; the thumb is always drawn.
    pub fn with_scrollbar_track_color(mut self, color: Color) -> Theme {
        self.0.color_scrollbar_track = color.raw();
        self
    }

    /// Set the scrollbar thumb colour at rest.
    pub fn with_scrollbar_thumb_color(mut self, color: Color) -> Theme {
        self.0.color_scrollbar_thumb = color.raw();
        self
    }

    /// Set the scrollbar thumb colour when the cursor hovers the track.
    pub fn with_scrollbar_thumb_hover_color(mut self, color: Color) -> Theme {
        self.0.color_scrollbar_thumb_hover = color.raw();
        self
    }

    /// Set the scrollbar thumb colour while the thumb is being dragged.
    pub fn with_scrollbar_thumb_active_color(mut self, color: Color) -> Theme {
        self.0.color_scrollbar_thumb_active = color.raw();
        self
    }

    /// Set the unfilled slider-track colour.
    pub fn with_slider_track_color(mut self, color: Color) -> Theme {
        self.0.color_slider_track = color.raw();
        self
    }

    /// Set the filled-range colour for horizontal and vertical sliders.
    pub fn with_slider_fill_color(mut self, color: Color) -> Theme {
        self.0.color_slider_fill = color.raw();
        self
    }

    /// Set the slider knob colour independently from its filled range.
    pub fn with_slider_knob_color(mut self, color: Color) -> Theme {
        self.0.color_slider_knob = color.raw();
        self
    }

    /// Set the slider track thickness in logical pixels.
    pub fn with_slider_track_thickness(mut self, thickness: f32) -> Theme {
        self.0.slider_track_thickness = thickness.max(0.0);
        self
    }

    /// Set the slider knob diameter in logical pixels.
    pub fn with_slider_knob_size(mut self, size: f32) -> Theme {
        self.0.slider_knob_size = size.max(0.0);
        self
    }

    /// Set the base UI font size.
    pub fn with_font_size(mut self, size: f32) -> Theme {
        self.0.font_size = size.max(1.0);
        self
    }

    /// Set the application body/background colour.
    pub fn with_bg(mut self, color: Color) -> Theme {
        self.0.color_bg = color.raw();
        self
    }

    /// Set the primary foreground/text colour.
    pub fn with_fg(mut self, color: Color) -> Theme {
        self.0.color_fg = color.raw();
        self
    }

    /// Set the accent colour used by focused and selected controls.
    pub fn with_accent(mut self, color: Color) -> Theme {
        self.0.color_accent = color.raw();
        self
    }

    /// Set the border and divider colour.
    pub fn with_border(mut self, color: Color) -> Theme {
        self.0.color_border = color.raw();
        self
    }

    /// Set the hover-surface colour.
    pub fn with_hover(mut self, color: Color) -> Theme {
        self.0.color_hover = color.raw();
        self
    }

    /// Set the pressed and selected-surface colour. Selectable rows use this
    /// colour whether or not an active-indicator rail is enabled.
    pub fn with_active(mut self, color: Color) -> Theme {
        self.0.color_active = color.raw();
        self
    }

    /// Set the disabled-control colour.
    pub fn with_disabled(mut self, color: Color) -> Theme {
        self.0.color_disabled = color.raw();
        self
    }

    /// Set the validation/error colour.
    pub fn with_error(mut self, color: Color) -> Theme {
        self.0.color_error = color.raw();
        self
    }

    /// Background colour of the window/body.
    pub fn bg(self) -> Color {
        Color(self.0.color_bg)
    }

    /// Foreground (text) colour.
    pub fn fg(self) -> Color {
        Color(self.0.color_fg)
    }

    /// Accent colour for interactive highlights.
    pub fn accent(self) -> Color {
        Color(self.0.color_accent)
    }

    /// Border/divider colour.
    pub fn border(self) -> Color {
        Color(self.0.color_border)
    }

    /// Hover state surface colour.
    pub fn hover(self) -> Color {
        Color(self.0.color_hover)
    }

    /// Active/pressed state colour.
    pub fn active(self) -> Color {
        Color(self.0.color_active)
    }

    /// Disabled control colour.
    pub fn disabled(self) -> Color {
        Color(self.0.color_disabled)
    }

    /// Error/invalid state colour.
    pub fn error(self) -> Color {
        Color(self.0.color_error)
    }

    /// Base body-font size in logical pixels.
    pub fn font_size(self) -> f32 {
        self.0.font_size
    }

    /// Shared internal control padding in logical pixels.
    pub fn padding(self) -> f32 {
        self.0.padding
    }

    /// Best-effort dark-mode detection from the body background luminance.
    /// True when the background is darker than the foreground.
    pub fn is_dark(self) -> bool {
        let (br, bg, bb, _) = self.bg().components();
        let (fr, fg, fb, _) = self.fg().components();
        // Rec. 601 luma.
        let bluma = 0.299 * br as f32 + 0.587 * bg as f32 + 0.114 * bb as f32;
        let fluma = 0.299 * fr as f32 + 0.587 * fg as f32 + 0.114 * fb as f32;
        bluma < fluma
    }
}

impl Default for Theme {
    fn default() -> Theme {
        Theme::light()
    }
}

/// A built-in vector icon glyph, for [`crate::Frame::icon`] and
/// [`crate::Frame::icon_button`]. Mirrors a subset of `lens_icon_id`; reach for
/// `sys::lens_icon_id` directly for ids not listed here.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[non_exhaustive]
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
}

impl Icon {
    pub(crate) fn raw(self) -> sys::lens_icon_id {
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
        }
    }
}

/// Options for a modal dialog ([`crate::Frame::modal`], ADR-0039).
///
/// A modal is a centered overlay with a dim backdrop that eclipses the base
/// tree, plus a Tab focus trap so keyboard cycling stays inside the dialog
/// body.
#[derive(Debug, Clone, Copy)]
pub struct ModalOpts<'a> {
    /// Optional heading drawn at the top of the dialog.
    pub title: Option<&'a str>,
    /// Dim colour over the base tree; [`Color::TRANSPARENT`] selects the
    /// library default (50% black).
    pub backdrop: Color,
    /// Content minimum width in logical px; 0 selects the library default.
    pub min_width: f32,
    /// When true (the default), Escape and click-outside close the dialog.
    pub dismissable: bool,
}

impl Default for ModalOpts<'_> {
    fn default() -> Self {
        ModalOpts {
            title: None,
            backdrop: Color::TRANSPARENT,
            min_width: 0.0,
            dismissable: true,
        }
    }
}

/// Appearance of a floating overlay layer ([`crate::Frame::overlay`]).
/// `Default` is a borderless, fill-less layer with small padding.
#[derive(Debug, Clone, Copy)]
pub struct OverlayOpts {
    pub gap: f32,
    pub pad: f32,
    pub cross: Align,
    /// Background fill; [`Color::TRANSPARENT`] means none.
    pub bg: Color,
    /// Border stroke; [`Color::TRANSPARENT`] means none.
    pub border: Color,
    pub border_width: f32,
    pub radius: f32,
    /// Sets a fixed minimum width on the layer when > 0.
    pub min_width: f32,
}

impl Default for OverlayOpts {
    fn default() -> OverlayOpts {
        OverlayOpts {
            gap: 4.0,
            pad: 6.0,
            cross: Align::Stretch,
            bg: Color::TRANSPARENT,
            border: Color::TRANSPARENT,
            border_width: 0.0,
            radius: 0.0,
            min_width: 0.0,
        }
    }
}

/// Container layout options for [`crate::Frame::row_ex`] /
/// [`crate::Frame::column_ex`]. Mirrors `lens_layout_opts`.
///
/// The plain [`crate::Frame::row`] / [`crate::Frame::column`] containers have
/// no gap, no padding, and stretch children on the cross axis; this descriptor
/// adds control over spacing, surface, and alignment — the difference between a
/// polished layout and widgets jammed into the top-left corner.
#[derive(Debug, Clone, Copy)]
pub struct LayoutOpts {
    /// Main-axis grow factor for the container itself (0 = intrinsic size).
    pub flex: f32,
    /// Fixed width in logical px (0 = intrinsic).
    pub width: f32,
    /// Fixed height in logical px (0 = intrinsic).
    pub height: f32,
    /// Minimum resolved width in logical px (0 = no lower bound).
    pub min_width: f32,
    /// Maximum resolved width in logical px (0 = no upper bound).
    pub max_width: f32,
    /// Minimum resolved height in logical px (0 = no lower bound).
    pub min_height: f32,
    /// Maximum resolved height in logical px (0 = no upper bound).
    pub max_height: f32,
    /// Gap between children along the main axis.
    pub gap: f32,
    /// Padding inside the container, applied to all four sides.
    pub pad: f32,
    /// Cross-axis alignment of children. [`Align::Stretch`] fills the cross
    /// axis; [`Align::Center`] sizes children to their intrinsic width and
    /// centres them.
    pub cross: Align,
    /// Background fill; [`Color::TRANSPARENT`] means no background.
    pub bg: Color,
    /// Corner radius of the background fill.
    pub radius: f32,
}

impl Default for LayoutOpts {
    fn default() -> LayoutOpts {
        LayoutOpts {
            flex: 0.0,
            width: 0.0,
            height: 0.0,
            min_width: 0.0,
            max_width: 0.0,
            min_height: 0.0,
            max_height: 0.0,
            gap: 0.0,
            pad: 0.0,
            cross: Align::Stretch,
            bg: Color::TRANSPARENT,
            radius: 0.0,
        }
    }
}

impl LayoutOpts {
    pub(crate) fn to_raw(self) -> sys::lens_layout_opts {
        sys::lens_layout_opts {
            box_: sys::lens_box {
                id: std::ptr::null(),
                flex: self.flex,
                width: self.width,
                height: self.height,
                disabled: false,
                error: false,
                tooltip: std::ptr::null(),
            },
            min_width: self.min_width,
            max_width: self.max_width,
            min_height: self.min_height,
            max_height: self.max_height,
            gap: self.gap,
            pad: self.pad,
            cross: self.cross.raw(),
            bg: self.bg.raw(),
            radius: self.radius,
        }
    }
}

impl OverlayOpts {
    pub(crate) fn to_raw(self) -> sys::lens_overlay_opts {
        sys::lens_overlay_opts {
            gap: self.gap,
            pad: self.pad,
            cross: self.cross.raw(),
            bg: self.bg.raw(),
            border: self.border.raw(),
            border_width: self.border_width,
            radius: self.radius,
            min_width: self.min_width,
        }
    }
}
