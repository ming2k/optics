//! Small value types shared across the safe surface: geometry, colour, the
//! interaction response, and placement options.

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
    /// Fully transparent (alpha 0). In place options this means "no fill".
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

/// Widget-kind tags for skin replacement ([`crate::Frame::set_skin`],
/// ADR-0059). Re-exported from the raw bindings: it is a plain fieldless
/// `u32` enum with no invalid states the C side would reject.
pub use sys::lens_widget_kind as WidgetKind;

/// Everything a skin needs to draw one widget for one frame (ADR-0059):
/// kind, state bits, node-local bounds, the resolved style, eased floats,
/// and the per-kind content payload. Raw bindgen layout mirroring
/// `lens_widget_record`; strings inside are borrowed for the frame.
pub use sys::lens_widget_record as WidgetRecord;

/// The per-kind content payload of a [`WidgetRecord`]. Raw bindgen layout
/// mirroring `lens_widget_content`; a member is meaningful only for the
/// kinds its C comment lists.
pub use sys::lens_widget_content as WidgetContent;

/// One wrapped/visible text line in a LABEL or TEXTAREA record payload.
/// Raw bindgen layout mirroring `lens_text_line`.
pub use sys::lens_text_line as TextLine;

/// One column band in a TABLE record payload. Raw bindgen layout mirroring
/// `lens_grid_column`.
pub use sys::lens_grid_column as GridColumn;

/// One visible row in a TABLE record payload. Raw bindgen layout mirroring
/// `lens_grid_row`.
pub use sys::lens_grid_row as GridRow;

/// A fully-resolved style (ADR-0058) as handed to skins in the record.
/// Raw bindgen layout mirroring `lens_style_resolved`.
pub use sys::lens_style_resolved as StyleResolved;

/// A skin function (ADR-0059): draws one widget for one frame from its
/// record. Raw `extern "C"` pointers for now — closure trampolines that
/// would let safe Rust closures act as skins are future work (noted in
/// the ADR). `None` restores/falls through to the default skin.
pub type SkinFn = sys::lens_skin_fn;

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
    /// Arrow-key/Home/End cursor plus Return activation while the
    /// table is focused (ADR-0066). Requires `selectable` — only selectable
    /// tables take focus.
    pub keyboard: bool,
}

/// Structural options for a tab strip (ADR-0061: the presentation knobs
/// retired into the style cascade and caller-owned skins; the default skin
/// draws the neutral static indicator).
#[derive(Debug, Clone, Copy, Default)]
pub struct TabsOpts {
    /// Give every tab the same share of the strip's available width.
    pub equal_width: bool,
}

impl TabsOpts {
    pub(crate) fn to_raw(self) -> sys::lens_tabs_opts {
        sys::lens_tabs_opts {
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
            keyboard: false,
        }
    }
}

/// Interaction result returned by [`crate::Frame::table`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct TableResult {
    pub selected: Option<usize>,
    pub selection_changed: bool,
    pub clicked: bool,
    /// Effective cursor row after this frame (ADR-0066).
    pub cursor: Option<usize>,
    /// The effective cursor moved during this frame.
    pub cursor_changed: bool,
    /// Return fired on the cursor row (keyboard mode) or an a11y
    /// DoAction fired this frame.
    pub activated: bool,
    /// Row clicked this frame.
    pub clicked_row: Option<usize>,
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
    /// a [`crate::PlaceOpts`] popup.
    pub rect: Rect,
    pub hovered: bool,
    pub pressed: bool,
    pub clicked: bool,
    pub right_clicked: bool,
    pub middle_clicked: bool,
    pub changed: bool,
    pub focused: bool,
    /// Explicit widget-state bits (ADR-0058): a bitflag superset of the
    /// boolean fields above, plus widget-owned bits like
    /// [`WidgetState::SELECTED`] and [`WidgetState::ACTIVE`].
    pub state: WidgetState,
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
            state: WidgetState(r.state),
        }
    }
}

/// Widget state bits, mirroring the C `lens_widget_state` flags (ADR-0058).
/// The interaction core reports hover/press/focus/disabled; widgets add the
/// bits only they know — selection, toggle on-state, dragging.
/// [`WidgetState::FOCUS_VISIBLE`] means focused via keyboard navigation
/// (Tab traversal); a pointer click focuses without it.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct WidgetState(pub u32);

impl WidgetState {
    pub const EMPTY: Self = Self(0);
    pub const HOVERED: Self = Self(sys::lens_widget_state::LENS_STATE_HOVERED as u32);
    pub const PRESSED: Self = Self(sys::lens_widget_state::LENS_STATE_PRESSED as u32);
    pub const FOCUSED: Self = Self(sys::lens_widget_state::LENS_STATE_FOCUSED as u32);
    pub const FOCUS_VISIBLE: Self = Self(sys::lens_widget_state::LENS_STATE_FOCUS_VISIBLE as u32);
    pub const DISABLED: Self = Self(sys::lens_widget_state::LENS_STATE_DISABLED as u32);
    pub const SELECTED: Self = Self(sys::lens_widget_state::LENS_STATE_SELECTED as u32);
    pub const ACTIVE: Self = Self(sys::lens_widget_state::LENS_STATE_ACTIVE as u32);
    pub const DRAGGED: Self = Self(sys::lens_widget_state::LENS_STATE_DRAGGED as u32);

    /// True when every bit in `other` is set.
    pub fn contains(self, other: Self) -> bool {
        self.0 & other.0 == other.0
    }

    /// True when any bit in `other` is set.
    pub fn intersects(self, other: Self) -> bool {
        self.0 & other.0 != 0
    }

    /// True when no bits are set.
    pub fn is_empty(self) -> bool {
        self.0 == 0
    }
}

impl std::ops::BitOr for WidgetState {
    type Output = Self;
    fn bitor(self, rhs: Self) -> Self {
        Self(self.0 | rhs.0)
    }
}

impl std::ops::BitOrAssign for WidgetState {
    fn bitor_assign(&mut self, rhs: Self) {
        self.0 |= rhs.0;
    }
}

impl std::ops::BitAnd for WidgetState {
    type Output = Self;
    fn bitand(self, rhs: Self) -> Self {
        Self(self.0 & rhs.0)
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

    /// Unfilled slider-track colour.
    pub fn slider_track_color(self) -> Color {
        Color(self.0.color_slider_track)
    }

    /// Filled-range colour for horizontal and vertical sliders.
    pub fn slider_fill_color(self) -> Color {
        Color(self.0.color_slider_fill)
    }

    /// Slider knob colour.
    pub fn slider_knob_color(self) -> Color {
        Color(self.0.color_slider_knob)
    }

    /// Scrollbar track fill colour.
    pub fn scrollbar_track_color(self) -> Color {
        Color(self.0.color_scrollbar_track)
    }

    /// Scrollbar thumb colour at rest.
    pub fn scrollbar_thumb_color(self) -> Color {
        Color(self.0.color_scrollbar_thumb)
    }

    /// Scrollbar thumb colour when the cursor hovers the track.
    pub fn scrollbar_thumb_hover_color(self) -> Color {
        Color(self.0.color_scrollbar_thumb_hover)
    }

    /// Scrollbar thumb colour while the thumb is being dragged.
    pub fn scrollbar_thumb_active_color(self) -> Color {
        Color(self.0.color_scrollbar_thumb_active)
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

/// A per-instance style override for a single widget call (ADR-0058),
/// wrapping `lens_style`. Every field is optional: only the fields set
/// through the `with_*` methods are marked in the C side's `fields` mask,
/// and unset fields fall back to the active theme inside the style
/// resolver — so `Style::default()` renders exactly the themed default.
/// State feedback comes along for free: setting [`Style::with_bg`] derives
/// the hover/pressed surfaces, and a disabled widget dims instance colours.
///
/// ```no_run
/// # let mut ui = lens::Ui::headless().unwrap();
/// # let input = lens::Input::default();
/// ui.frame(&input, |f| {
///     let danger = lens::Style::default().with_bg(lens::Color::rgba(0xc0, 0x20, 0x20, 0xff));
///     f.push_style(danger); // every widget declared inside the scope
///     if f.button("Delete") { /* ... */ }
///     f.pop_style();
/// });
/// ```
#[derive(Debug, Clone, Copy, Default)]
pub struct Style(pub(crate) sys::lens_style);

impl Style {
    /// An empty style: nothing set, everything resolves to the theme.
    pub fn new() -> Style {
        Style::default()
    }

    /// Resting surface colour. For a button this replaces the accent body.
    pub fn with_bg(mut self, color: Color) -> Style {
        self.0.bg = color.raw();
        self.0.fields |= sys::lens_style_field::LENS_STYLE_BG as u32;
        self
    }

    /// Hovered surface colour. Derived from `bg` when only `bg` is set.
    pub fn with_bg_hover(mut self, color: Color) -> Style {
        self.0.bg_hover = color.raw();
        self.0.fields |= sys::lens_style_field::LENS_STYLE_BG_HOVER as u32;
        self
    }

    /// Pressed/selected surface colour (theme: `color_active`). A selected
    /// selectable row paints this; derived from `bg` when only `bg` is set.
    pub fn with_bg_pressed(mut self, color: Color) -> Style {
        self.0.bg_pressed = color.raw();
        self.0.fields |= sys::lens_style_field::LENS_STYLE_BG_PRESSED as u32;
        self
    }

    /// Foreground colour for text and glyphs.
    pub fn with_fg(mut self, color: Color) -> Style {
        self.0.fg = color.raw();
        self.0.fields |= sys::lens_style_field::LENS_STYLE_FG as u32;
        self
    }

    /// Border stroke colour.
    pub fn with_border(mut self, color: Color) -> Style {
        self.0.border = color.raw();
        self.0.fields |= sys::lens_style_field::LENS_STYLE_BORDER as u32;
        self
    }

    /// Accent colour (rails, emphasis).
    pub fn with_accent(mut self, color: Color) -> Style {
        self.0.accent = color.raw();
        self.0.fields |= sys::lens_style_field::LENS_STYLE_ACCENT as u32;
        self
    }

    /// Corner radius in logical pixels.
    pub fn with_corner_radius(mut self, radius: f32) -> Style {
        self.0.corner_radius = radius.max(0.0);
        self.0.fields |= sys::lens_style_field::LENS_STYLE_CORNER_RADIUS as u32;
        self
    }

    /// Border stroke width in logical pixels.
    pub fn with_border_width(mut self, width: f32) -> Style {
        self.0.border_width = width.max(0.0);
        self.0.fields |= sys::lens_style_field::LENS_STYLE_BORDER_WIDTH as u32;
        self
    }

    /// Internal padding in logical pixels.
    pub fn with_padding(mut self, padding: f32) -> Style {
        self.0.padding = padding.max(0.0);
        self.0.fields |= sys::lens_style_field::LENS_STYLE_PADDING as u32;
        self
    }

    /// Inter-child gap in logical pixels.
    pub fn with_gap(mut self, gap: f32) -> Style {
        self.0.gap = gap.max(0.0);
        self.0.fields |= sys::lens_style_field::LENS_STYLE_GAP as u32;
        self
    }

    /// Font size in logical pixels.
    pub fn with_font_size(mut self, size: f32) -> Style {
        self.0.font_size = size.max(1.0);
        self.0.fields |= sys::lens_style_field::LENS_STYLE_FONT_SIZE as u32;
        self
    }

    /// Contour colour behind foreground content (text, glyphs, images)
    /// floating over imagery or translucent material (ADR-0061: the retired
    /// `*_outlined` variants' effect as a style atom).
    pub fn with_outline_color(mut self, color: Color) -> Style {
        self.0.outline_color = color.raw();
        self.0.fields |= sys::lens_style_field::LENS_STYLE_OUTLINE_COLOR as u32;
        self
    }

    /// Contour radius in logical pixels; see [`Style::with_outline_color`].
    pub fn with_outline_width(mut self, width: f32) -> Style {
        self.0.outline_width = width.max(0.0);
        self.0.fields |= sys::lens_style_field::LENS_STYLE_OUTLINE_WIDTH as u32;
        self
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
/// A modal is a centered transient popup with a dim backdrop that occludes
/// the base tree, plus a Tab focus trap so keyboard cycling stays inside
/// the dialog body.
#[derive(Debug, Clone, Copy)]
pub struct ModalOpts<'a> {
    /// Optional heading drawn at the top of the dialog.
    pub title: Option<&'a str>,
    /// Dim colour over the base tree; [`Color::TRANSPARENT`] selects the
    /// library default (50% black).
    pub backdrop: Color,
    /// Content minimum width in logical px; 0 selects the library default.
    pub min_width: f32,
    /// When true, the dialog is pinned: Escape and click-outside do NOT
    /// close it. The default is false (dismissable).
    pub pinned: bool,
}

impl Default for ModalOpts<'_> {
    fn default() -> Self {
        ModalOpts {
            title: None,
            backdrop: Color::TRANSPARENT,
            min_width: 0.0,
            pinned: false,
        }
    }
}

/// A closed z band for an absolutely-placed subtree ([`crate::Frame::place`],
/// ADR-0060). There is no numeric z, ever: within a band, later registration
/// paints (and hit-tests) above earlier. Flow content is always
/// [`Band::Base`].
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum Band {
    /// Below the base tree; hit-transparent by default (see
    /// [`PlaceOpts::interactive`]).
    Backdrop,
    /// The base tree itself (flow nodes live here).
    Base,
    /// Persistent chrome: docks, status bars.
    Chrome,
    /// Transient popups: dropdowns, menus, modals.
    #[default]
    Popup,
    /// Above everything: drag ghosts, tooltips.
    Topmost,
}

impl Band {
    pub(crate) fn raw(self) -> sys::lens_band {
        match self {
            Band::Backdrop => sys::lens_band::LENS_BAND_BACKDROP,
            Band::Base => sys::lens_band::LENS_BAND_BASE,
            Band::Chrome => sys::lens_band::LENS_BAND_CHROME,
            Band::Popup => sys::lens_band::LENS_BAND_POPUP,
            Band::Topmost => sys::lens_band::LENS_BAND_TOPMOST,
        }
    }
}

/// How a placed subtree's `rect`/`bounds` resolve to a position
/// ([`crate::Frame::place`], ADR-0060).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum PlaceMode {
    /// `rect` is the top-left position plus a minimum extent (persistent
    /// chrome, scrims); clamped to the bounds.
    Exact,
    /// `rect` is the owner anchor: probe at it, drop below, flip above on
    /// overflow, clamp (dropdowns, menus).
    #[default]
    Anchored,
    /// Centred on the bounds; `rect` ignored (modal dialogs).
    Centered,
}

impl PlaceMode {
    pub(crate) fn raw(self) -> sys::lens_place_mode {
        match self {
            PlaceMode::Exact => sys::lens_place_mode::LENS_PLACE_EXACT,
            PlaceMode::Anchored => sys::lens_place_mode::LENS_PLACE_ANCHORED,
            PlaceMode::Centered => sys::lens_place_mode::LENS_PLACE_CENTERED,
        }
    }
}

/// Placement of an absolutely-positioned container sub-root
/// ([`crate::Frame::place`], ADR-0060). `Default` is an anchored, transient
/// POPUP with small padding — the dropdown/menu case.
#[derive(Debug, Clone, Copy)]
pub struct PlaceOpts {
    /// Z band for the subtree.
    pub band: Band,
    /// How `rect`/`bounds` resolve to a position.
    pub mode: PlaceMode,
    /// [`PlaceMode::Exact`]: top-left + minimum extent;
    /// [`PlaceMode::Anchored`]: owner anchor (counts as inside for
    /// click-outside dismissal); [`PlaceMode::Centered`]: ignored.
    pub rect: Rect,
    /// Placement + render boundary; zero-sized means the display.
    pub bounds: Rect,
    /// Open-set managed: the body builds only while the id is open
    /// ([`crate::Frame::place_open`]); Escape and click-outside dismiss it.
    pub transient: bool,
    /// [`Band::Backdrop`] only: opt into hit-testing. All other bands
    /// always occlude/hit normally.
    pub interactive: bool,
    /// The subtree's internal flexbox + surface (gap/pad/cross, bg, border,
    /// radius; `min_width` > 0 fixes the node's width, as do `width` /
    /// an EXACT `rect.w`).
    pub layout: LayoutOpts,
}

impl Default for PlaceOpts {
    fn default() -> PlaceOpts {
        PlaceOpts {
            band: Band::Popup,
            mode: PlaceMode::Anchored,
            rect: Rect {
                x: 0.0,
                y: 0.0,
                w: 0.0,
                h: 0.0,
            },
            bounds: Rect {
                x: 0.0,
                y: 0.0,
                w: 0.0,
                h: 0.0,
            },
            transient: true,
            interactive: false,
            layout: LayoutOpts {
                gap: 4.0,
                pad: 6.0,
                cross: Align::Stretch,
                ..LayoutOpts::default()
            },
        }
    }
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
    /// Border stroke; [`Color::TRANSPARENT`] means none.
    pub border: Color,
    /// Border stroke width when `border` is visible.
    pub border_width: f32,
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
            border: Color::TRANSPARENT,
            border_width: 0.0,
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
                style: sys::lens_style::default(),
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
            border: self.border.raw(),
            border_width: self.border_width,
        }
    }
}
