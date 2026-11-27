//! Canvas drawing commands.

use super::*;

/// Orthogonal configuration for [`CanvasCommands::draw_image_with`].
///
/// Any combination of sampler override, normalized source sub-rectangle,
/// destination rounded/squircle geometry, independent rounded/squircle clip,
/// tint, blend mode, and opaque-alpha override composes cleanly in one call.
#[derive(Clone, Copy, Debug)]
pub struct ImageDrawOpts<'a> {
    /// Custom sampler; `None` uses the canvas default (linear).
    pub sampler: Option<&'a Sampler>,
    /// Normalized source UV sub-rectangle `(u, v, du, dv)`; `None` samples the whole image.
    pub src: Option<(f32, f32, f32, f32)>,
    /// Independent clip rectangle `(x, y, w, h)` in canvas units; `None` disables independent clip.
    pub clip: Option<(f32, f32, f32, f32)>,
    /// Per-corner radii for `clip` when `clip.is_some()`.
    pub clip_radii: CornerRadii,
    /// Corner curvature (`0.0` = circular, `1.0` = G2 squircle) for `clip`.
    pub clip_curvature: f32,
    /// Per-corner radii for the destination rectangle.
    pub radii: CornerRadii,
    /// Corner curvature (`0.0` = circular, `1.0` = G2 squircle) for the destination rectangle.
    pub curvature: f32,
    /// Premultiplied tint color (default: opaque white `0xFFFFFFFF`).
    pub tint: u32,
    /// Fixed-function blend mode (default: [`BlendMode::SrcOver`]).
    pub blend: BlendMode,
    /// Force alpha to opaque (`1.0`), replacing destination with `Src` when drawing alpha-free RGB images.
    pub opaque_only: bool,
}

impl Default for ImageDrawOpts<'_> {
    fn default() -> Self {
        Self {
            sampler: None,
            src: None,
            clip: None,
            clip_radii: CornerRadii::ZERO,
            clip_curvature: 0.0,
            radii: CornerRadii::ZERO,
            curvature: 0.0,
            tint: 0xFFFFFFFF,
            blend: BlendMode::SrcOver,
            opaque_only: false,
        }
    }
}

impl CanvasCommands<'_> {
    /// Submit a compiled DisplayList for hardware execution (ADR-0088 / ADR-0090).
    pub fn submit_display_list(&self, list: &DisplayList) -> Result<(), Error> {
        let opts = sys::optics_gfx_canvas_submit_display_list_opts {
            list: list.as_raw(),
            bindings: std::ptr::null(),
            binding_count: 0,
        };
        Error::check(unsafe { sys::optics_gfx_canvas_submit_display_list(self.raw, &opts) })
    }

    /// Submit a DisplayList after binding every dynamic image input slot.
    pub fn submit_display_list_bound(
        &self,
        list: &DisplayList,
        bindings: &[DisplayListBinding<'_>],
    ) -> Result<(), Error> {
        let raw: Vec<sys::optics_gfx_display_list_binding> = bindings
            .iter()
            .map(|binding| sys::optics_gfx_display_list_binding {
                slot: binding.slot,
                image: binding.image.as_raw(),
                sampler: binding
                    .sampler
                    .map(|sampler| sampler.as_raw())
                    .unwrap_or(std::ptr::null_mut()),
            })
            .collect();
        let opts = sys::optics_gfx_canvas_submit_display_list_opts {
            list: list.as_raw(),
            bindings: raw.as_ptr(),
            binding_count: raw.len() as u32,
        };
        Error::check(unsafe { sys::optics_gfx_canvas_submit_display_list(self.raw, &opts) })
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn draw_image_rect_internal(
        &self,
        image: *mut sys::optics_gfx_image,
        sampler: *mut sys::optics_gfx_sampler,
        dst: sys::optics_gfx_rect,
        src: sys::optics_gfx_rect,
        clip: sys::optics_gfx_rect,
        clip_radii: CornerRadii,
        clip_curvature: f32,
        rrect_radii: CornerRadii,
        rrect_curvature: f32,
        tint: u32,
        blend: sys::optics_gfx_blend_mode,
        opaque_only: bool,
    ) {
        let is_rrect = !rrect_radii.is_zero();
        let geom = sys::optics_gfx_geometry {
            kind: if is_rrect {
                sys::optics_gfx_geom_kind::OPTICS_GFX_GEOM_RRECT
            } else {
                sys::optics_gfx_geom_kind::OPTICS_GFX_GEOM_RECT
            },
            _pad: [0; 3],
            stroke: sys::optics_gfx_stroke_style {
                width: 0.0,
                ..Default::default()
            },
            __bindgen_anon_1: if is_rrect {
                sys::optics_gfx_geometry__bindgen_ty_1 {
                    rrect: sys::optics_gfx_geom_rrect_data {
                        rect: dst,
                        radii: rrect_radii.to_raw(),
                        curvature: clamp_curvature(rrect_curvature),
                    },
                }
            } else {
                sys::optics_gfx_geometry__bindgen_ty_1 {
                    rect: sys::optics_gfx_geom_rect_data { rect: dst },
                }
            },
        };
        let brush = sys::optics_gfx_brush {
            kind: sys::optics_gfx_brush_kind::OPTICS_GFX_BRUSH_IMAGE_PATTERN,
            blend,
            opacity: 1.0,
            __bindgen_anon_1: sys::optics_gfx_brush__bindgen_ty_1 {
                image: sys::optics_gfx_brush_image_data {
                    image,
                    sampler,
                    src_rect: src,
                    opaque_only,
                    tint,
                    clip_rect: clip,
                    clip_radii: clip_radii.to_raw(),
                    clip_curvature: clamp_curvature(clip_curvature),
                },
            },
        };
        let opts = sys::optics_gfx_canvas_draw_geometry_opts {
            geom: &geom,
            brush: &brush,
        };
        unsafe { sys::optics_gfx_canvas_draw_geometry(self.raw, &opts) };
    }

    /// Save the current transform/clip state onto the stack.
    pub fn save(&self) {
        unsafe { sys::optics_gfx_canvas_save(self.raw) };
    }

    /// Restore the transform/clip state saved by the matching [`save`](Self::save).
    pub fn restore(&self) {
        unsafe { sys::optics_gfx_canvas_restore(self.raw) };
    }

    /// Translate the coordinate system by `(x, y)`.
    pub fn translate(&self, x: f32, y: f32) {
        let opts = sys::optics_gfx_canvas_translate_opts { x, y };
        unsafe { sys::optics_gfx_canvas_translate(self.raw, &opts) };
    }

    /// Scale the coordinate system by `(sx, sy)`. Used to map a logical
    /// (DPI-independent) coordinate space onto the physical framebuffer: pair
    /// `scale(s, s)` with the text context's device scale `s` for crisp HiDPI text.
    pub fn scale(&self, sx: f32, sy: f32) {
        let opts = sys::optics_gfx_canvas_scale_opts { sx, sy };
        unsafe { sys::optics_gfx_canvas_scale(self.raw, &opts) };
    }

    /// Rotate subsequent drawing by `radians` around the current origin.
    pub fn rotate(&self, radians: f32) {
        unsafe { sys::optics_gfx_canvas_rotate(self.raw, radians) };
    }

    /// Intersect subsequent drawing with an axis-aligned clip rectangle.
    pub fn clip_rect(&self, x: f32, y: f32, w: f32, h: f32) {
        let rect = sys::optics_gfx_rect { x, y, w, h };
        unsafe { sys::optics_gfx_canvas_clip_rect(self.raw, rect) };
    }

    /// Fill a rectangle with a packed solid color.
    pub fn fill_rect(&self, x: f32, y: f32, w: f32, h: f32, color: u32) {
        let geom = sys::optics_gfx_geometry {
            kind: sys::optics_gfx_geom_kind::OPTICS_GFX_GEOM_RECT,
            _pad: [0; 3],
            stroke: sys::optics_gfx_stroke_style {
                width: 0.0,
                ..Default::default()
            },
            __bindgen_anon_1: sys::optics_gfx_geometry__bindgen_ty_1 {
                rect: sys::optics_gfx_geom_rect_data {
                    rect: sys::optics_gfx_rect { x, y, w, h },
                },
            },
        };
        let brush = sys::optics_gfx_brush {
            kind: sys::optics_gfx_brush_kind::OPTICS_GFX_BRUSH_SOLID,
            blend: sys::optics_gfx_blend_mode::OPTICS_GFX_BLEND_SRC_OVER,
            opacity: 1.0,
            __bindgen_anon_1: sys::optics_gfx_brush__bindgen_ty_1 {
                solid: sys::optics_gfx_brush_solid_data { color },
            },
        };
        let opts = sys::optics_gfx_canvas_draw_geometry_opts {
            geom: &geom,
            brush: &brush,
        };
        unsafe { sys::optics_gfx_canvas_draw_geometry(self.raw, &opts) };
    }

    /// Fill a rounded rectangle or continuous-curvature squircle with a packed
    /// solid color (analytic-AA SDF). Accepts `f32` (uniform), `[f32; 4]`, or
    /// [`CornerRadii`]; `curvature == 0.0` is circular, `1.0` is G2 squircle.
    #[allow(clippy::too_many_arguments)]
    pub fn fill_rrect(
        &self,
        x: f32,
        y: f32,
        w: f32,
        h: f32,
        radii: impl Into<CornerRadii>,
        curvature: f32,
        color: u32,
    ) {
        let geom = sys::optics_gfx_geometry {
            kind: sys::optics_gfx_geom_kind::OPTICS_GFX_GEOM_RRECT,
            _pad: [0; 3],
            stroke: sys::optics_gfx_stroke_style {
                width: 0.0,
                ..Default::default()
            },
            __bindgen_anon_1: sys::optics_gfx_geometry__bindgen_ty_1 {
                rrect: sys::optics_gfx_geom_rrect_data {
                    rect: sys::optics_gfx_rect { x, y, w, h },
                    radii: radii.into().to_raw(),
                    curvature: clamp_curvature(curvature),
                },
            },
        };
        let brush = sys::optics_gfx_brush {
            kind: sys::optics_gfx_brush_kind::OPTICS_GFX_BRUSH_SOLID,
            blend: sys::optics_gfx_blend_mode::OPTICS_GFX_BLEND_SRC_OVER,
            opacity: 1.0,
            __bindgen_anon_1: sys::optics_gfx_brush__bindgen_ty_1 {
                solid: sys::optics_gfx_brush_solid_data { color },
            },
        };
        let opts = sys::optics_gfx_canvas_draw_geometry_opts {
            geom: &geom,
            brush: &brush,
        };
        unsafe { sys::optics_gfx_canvas_draw_geometry(self.raw, &opts) };
    }

    /// Stroke an analytic rounded rectangle or continuous-curvature squircle
    /// with a premultiplied colour.
    #[allow(clippy::too_many_arguments)]
    pub fn stroke_rrect(
        &self,
        x: f32,
        y: f32,
        w: f32,
        h: f32,
        radii: impl Into<CornerRadii>,
        curvature: f32,
        color: u32,
        width: f32,
    ) {
        let geom = sys::optics_gfx_geometry {
            kind: sys::optics_gfx_geom_kind::OPTICS_GFX_GEOM_RRECT,
            _pad: [0; 3],
            stroke: sys::optics_gfx_stroke_style {
                width,
                ..Default::default()
            },
            __bindgen_anon_1: sys::optics_gfx_geometry__bindgen_ty_1 {
                rrect: sys::optics_gfx_geom_rrect_data {
                    rect: sys::optics_gfx_rect { x, y, w, h },
                    radii: radii.into().to_raw(),
                    curvature: clamp_curvature(curvature),
                },
            },
        };
        let brush = sys::optics_gfx_brush {
            kind: sys::optics_gfx_brush_kind::OPTICS_GFX_BRUSH_SOLID,
            blend: sys::optics_gfx_blend_mode::OPTICS_GFX_BLEND_SRC_OVER,
            opacity: 1.0,
            __bindgen_anon_1: sys::optics_gfx_brush__bindgen_ty_1 {
                solid: sys::optics_gfx_brush_solid_data { color },
            },
        };
        let opts = sys::optics_gfx_canvas_draw_geometry_opts {
            geom: &geom,
            brush: &brush,
        };
        unsafe { sys::optics_gfx_canvas_draw_geometry(self.raw, &opts) };
    }

    /// Fill `path` with `paint` (the paint's fill rule applies).
    pub fn fill_path(&self, path: &Path<'_>, rule: FillRule, brush: &Brush) {
        let geom = sys::optics_gfx_geometry {
            kind: sys::optics_gfx_geom_kind::OPTICS_GFX_GEOM_PATH,
            _pad: [0; 3],
            stroke: sys::optics_gfx_stroke_style {
                width: 0.0,
                ..Default::default()
            },
            __bindgen_anon_1: sys::optics_gfx_geometry__bindgen_ty_1 {
                path: sys::optics_gfx_geom_path_data {
                    path: path.raw,
                    fill_rule: rule.raw(),
                },
            },
        };
        let brush = brush.raw;
        let opts = sys::optics_gfx_canvas_draw_geometry_opts {
            geom: &geom,
            brush: &brush,
        };
        unsafe { sys::optics_gfx_canvas_draw_geometry(self.raw, &opts) };
    }

    /// Stroke `path` with `paint` (width, cap, and join apply).
    pub fn stroke_path(&self, path: &Path<'_>, stroke: StrokeStyle, brush: &Brush) {
        let geom = sys::optics_gfx_geometry {
            kind: sys::optics_gfx_geom_kind::OPTICS_GFX_GEOM_PATH,
            _pad: [0; 3],
            stroke: stroke.raw(),
            __bindgen_anon_1: sys::optics_gfx_geometry__bindgen_ty_1 {
                path: sys::optics_gfx_geom_path_data {
                    path: path.raw,
                    fill_rule: sys::optics_gfx_fill_rule::OPTICS_GFX_FILL_NON_ZERO,
                },
            },
        };
        let brush = brush.raw;
        let opts = sys::optics_gfx_canvas_draw_geometry_opts {
            geom: &geom,
            brush: &brush,
        };
        unsafe { sys::optics_gfx_canvas_draw_geometry(self.raw, &opts) };
    }

    /// Fill a rectangle with a linear gradient in canvas pixel space.
    ///
    /// gfx supports at most eight stops; additional stops are ignored.
    pub fn fill_rect_linear_gradient(
        &self,
        rect: (f32, f32, f32, f32),
        from: (f32, f32),
        to: (f32, f32),
        stops: &[GradientStop],
    ) {
        let raw_stops = gradient_stops(stops);
        let geom = sys::optics_gfx_geometry {
            kind: sys::optics_gfx_geom_kind::OPTICS_GFX_GEOM_RECT,
            _pad: [0; 3],
            stroke: sys::optics_gfx_stroke_style {
                width: 0.0,
                ..Default::default()
            },
            __bindgen_anon_1: sys::optics_gfx_geometry__bindgen_ty_1 {
                rect: sys::optics_gfx_geom_rect_data {
                    rect: sys::optics_gfx_rect {
                        x: rect.0,
                        y: rect.1,
                        w: rect.2,
                        h: rect.3,
                    },
                },
            },
        };
        let mut stops_arr = [sys::optics_gfx_gradient_stop { t: 0.0, color: 0 }; 8];
        let count = raw_stops.len().min(8);
        stops_arr[..count].copy_from_slice(&raw_stops[..count]);
        let brush = sys::optics_gfx_brush {
            kind: sys::optics_gfx_brush_kind::OPTICS_GFX_BRUSH_LINEAR_GRADIENT,
            blend: sys::optics_gfx_blend_mode::OPTICS_GFX_BLEND_SRC_OVER,
            opacity: 1.0,
            __bindgen_anon_1: sys::optics_gfx_brush__bindgen_ty_1 {
                gradient: sys::optics_gfx_brush_gradient_data {
                    stops: sys::optics_gfx_gradient_stops {
                        stops: stops_arr,
                        count: raw_stops.len().try_into().unwrap_or(u32::MAX),
                    },
                    start: sys::optics_gfx_point {
                        x: from.0,
                        y: from.1,
                    },
                    end: sys::optics_gfx_point { x: to.0, y: to.1 },
                    radius: 0.0,
                },
            },
        };
        let opts = sys::optics_gfx_canvas_draw_geometry_opts {
            geom: &geom,
            brush: &brush,
        };
        unsafe { sys::optics_gfx_canvas_draw_geometry(self.raw, &opts) };
    }

    /// Fill a rectangle with a radial gradient in canvas pixel space.
    ///
    /// gfx supports at most eight stops; additional stops are ignored.
    pub fn fill_rect_radial_gradient(
        &self,
        rect: (f32, f32, f32, f32),
        center: (f32, f32),
        radius: f32,
        stops: &[GradientStop],
    ) {
        let raw_stops = gradient_stops(stops);
        let geom = sys::optics_gfx_geometry {
            kind: sys::optics_gfx_geom_kind::OPTICS_GFX_GEOM_RECT,
            _pad: [0; 3],
            stroke: sys::optics_gfx_stroke_style {
                width: 0.0,
                ..Default::default()
            },
            __bindgen_anon_1: sys::optics_gfx_geometry__bindgen_ty_1 {
                rect: sys::optics_gfx_geom_rect_data {
                    rect: sys::optics_gfx_rect {
                        x: rect.0,
                        y: rect.1,
                        w: rect.2,
                        h: rect.3,
                    },
                },
            },
        };
        let mut stops_arr = [sys::optics_gfx_gradient_stop { t: 0.0, color: 0 }; 8];
        let count = raw_stops.len().min(8);
        stops_arr[..count].copy_from_slice(&raw_stops[..count]);
        let brush = sys::optics_gfx_brush {
            kind: sys::optics_gfx_brush_kind::OPTICS_GFX_BRUSH_RADIAL_GRADIENT,
            blend: sys::optics_gfx_blend_mode::OPTICS_GFX_BLEND_SRC_OVER,
            opacity: 1.0,
            __bindgen_anon_1: sys::optics_gfx_brush__bindgen_ty_1 {
                gradient: sys::optics_gfx_brush_gradient_data {
                    stops: sys::optics_gfx_gradient_stops {
                        stops: stops_arr,
                        count: raw_stops.len().try_into().unwrap_or(u32::MAX),
                    },
                    start: sys::optics_gfx_point {
                        x: center.0,
                        y: center.1,
                    },
                    end: sys::optics_gfx_point { x: 0.0, y: 0.0 },
                    radius: radius.max(0.0),
                },
            },
        };
        let opts = sys::optics_gfx_canvas_draw_geometry_opts {
            geom: &geom,
            brush: &brush,
        };
        unsafe { sys::optics_gfx_canvas_draw_geometry(self.raw, &opts) };
    }

    /// Draw an image into the destination rectangle (pixel space) with default options.
    pub fn draw_image(&self, image: &Image, x: f32, y: f32, w: f32, h: f32) {
        self.draw_image_with(image, (x, y, w, h), &ImageDrawOpts::default());
    }

    /// Draw an image into `dst = (x, y, w, h)` with orthogonal sampling,
    /// sub-rectangle, rounded/squircle geometry or clip, tint, and blend options.
    pub fn draw_image_with(
        &self,
        image: &Image,
        dst: (f32, f32, f32, f32),
        opts: &ImageDrawOpts<'_>,
    ) {
        let src = opts.src.unwrap_or((0.0, 0.0, 0.0, 0.0));
        let clip = opts.clip.unwrap_or((0.0, 0.0, 0.0, 0.0));
        let blend = if opts.opaque_only {
            sys::optics_gfx_blend_mode::OPTICS_GFX_BLEND_SRC
        } else {
            opts.blend.raw()
        };
        self.draw_image_rect_internal(
            image.raw,
            opts.sampler.map(|s| s.raw).unwrap_or(std::ptr::null_mut()),
            sys::optics_gfx_rect {
                x: dst.0,
                y: dst.1,
                w: dst.2,
                h: dst.3,
            },
            sys::optics_gfx_rect {
                x: src.0,
                y: src.1,
                w: src.2,
                h: src.3,
            },
            sys::optics_gfx_rect {
                x: clip.0,
                y: clip.1,
                w: clip.2,
                h: clip.3,
            },
            opts.clip_radii,
            opts.clip_curvature,
            opts.radii,
            opts.curvature,
            opts.tint,
            blend,
            opts.opaque_only,
        );
    }

    /// The underlying raw `optics_gfx_canvas` pointer. Borrowed; the `Canvas`
    /// retains ownership.
    pub fn as_raw(&self) -> *mut sys::optics_gfx_canvas {
        self.raw
    }
    /// Wait a producer sync-file before this frame samples an already
    /// imported dma-buf image.
    ///
    /// Call during an active session (between [`Canvas::begin_session`] and
    /// [`CanvasSession::end`]), before drawing the image. gfx consumes
    /// `acquire_fence` on success; Rust
    /// closes it on error. The wait is submitted to the GPU and does not
    /// block the caller.
    pub fn wait_dmabuf_acquire(&self, image: &Image, acquire_fence: OwnedFd) -> Result<(), Error> {
        let opts = sys::optics_gfx_canvas_wait_dmabuf_acquire_opts {
            image: image.raw,
            acquire_sync_fd: acquire_fence.as_raw_fd(),
        };
        let result = unsafe { sys::optics_gfx_canvas_wait_dmabuf_acquire(self.raw, &opts) };
        Error::check(result)?;
        std::mem::forget(acquire_fence);
        Ok(())
    }
}
