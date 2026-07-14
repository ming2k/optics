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

    /// Set the active-indicator width used by ghost icon buttons
    /// ([`crate::Frame::icon_button_active`]). When > 0 the active state
    /// draws a left accent bar of this width and tints the glyph with the
    /// accent colour. When 0 the indicator is suppressed and only the
    /// background tint remains — useful for a calmer, tint-only active
    /// style. The background tint is drawn regardless of this value.
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

    /// Set the pressed/active-surface colour.
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

/// A built-in icon glyph (Feather set), for [`crate::Frame::icon`] and
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
    Menu,
    FileText,
    FilePlus,
    Edit,
    Plus,
    Trash,
    ChevronLeft,
    ChevronRight,
    Search,
    Star,
    X,
}

impl Icon {
    pub(crate) fn raw(self) -> sys::lens_icon_id {
        use sys::lens_icon_id::*;
        match self {
            Icon::Activity => LENS_ICON_ACTIVITY,
            Icon::BarChart => LENS_ICON_BAR_CHART_2,
            Icon::Bell => LENS_ICON_BELL,
            Icon::BookOpen => LENS_ICON_BOOK_OPEN,
            Icon::Briefcase => LENS_ICON_BRIEFCASE,
            Icon::CheckCircle => LENS_ICON_CHECK_CIRCLE,
            Icon::Clock => LENS_ICON_CLOCK,
            Icon::Database => LENS_ICON_DATABASE,
            Icon::DollarSign => LENS_ICON_DOLLAR_SIGN,
            Icon::ExternalLink => LENS_ICON_EXTERNAL_LINK,
            Icon::Globe => LENS_ICON_GLOBE,
            Icon::Grid => LENS_ICON_GRID,
            Icon::Home => LENS_ICON_HOME,
            Icon::Link => LENS_ICON_LINK,
            Icon::MessageCircle => LENS_ICON_MESSAGE_CIRCLE,
            Icon::Radio => LENS_ICON_RADIO,
            Icon::RefreshCw => LENS_ICON_REFRESH_CW,
            Icon::Rss => LENS_ICON_RSS,
            Icon::Shield => LENS_ICON_SHIELD,
            Icon::Target => LENS_ICON_TARGET,
            Icon::TrendingUp => LENS_ICON_TRENDING_UP,
            Icon::Users => LENS_ICON_USERS,
            Icon::Zap => LENS_ICON_ZAP,
            Icon::Settings => LENS_ICON_SETTINGS,
            Icon::Sidebar => LENS_ICON_SIDEBAR,
            Icon::Menu => LENS_ICON_MENU,
            Icon::FileText => LENS_ICON_FILE_TEXT,
            Icon::FilePlus => LENS_ICON_FILE_PLUS,
            Icon::Edit => LENS_ICON_EDIT,
            Icon::Plus => LENS_ICON_PLUS,
            Icon::Trash => LENS_ICON_TRASH_2,
            Icon::ChevronLeft => LENS_ICON_CHEVRON_LEFT,
            Icon::ChevronRight => LENS_ICON_CHEVRON_RIGHT,
            Icon::Search => LENS_ICON_SEARCH,
            Icon::Star => LENS_ICON_STAR,
            Icon::X => LENS_ICON_X,
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
