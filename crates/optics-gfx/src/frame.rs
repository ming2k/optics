//! Frame recording.

use super::*;

/// A recording frame obtained from [`Surface::begin_frame`]. It borrows the
/// surface because the underlying C frame is stored inside that surface rather
/// than being independently refcounted.
///
/// The frame API uses typestate: submission consumes this value and returns a
/// [`SubmittedFrame`], which is the only type that can be presented. Duplicate
/// and out-of-order transitions therefore cannot be expressed through safe
/// Rust.
#[must_use = "a frame must be submitted and then presented"]
pub struct Frame<'surface> {
    pub(crate) raw: *mut sys::optics_gfx_frame,
    pub(crate) surface: &'surface Surface,
}

impl<'surface> Frame<'surface> {
    /// Copy this exact frame into immutable readback staging during submit.
    /// Later frames do not change the snapshot.
    pub fn request_readback(&mut self) -> Result<(), Error> {
        Error::check(unsafe { sys::optics_gfx_frame_request_readback(self.raw) })
    }

    /// Copy only `region` from this frame into tightly packed immutable
    /// readback staging. The Vulkan copy and later CPU normalization are both
    /// proportional to the selected extent; no full-surface intermediate is
    /// produced.
    pub fn request_readback_region(&mut self, region: ReadbackRegion) -> Result<(), Error> {
        let raw = region.raw();
        Error::check(unsafe { sys::optics_gfx_frame_request_readback_region(self.raw, &raw) })
    }

    /// Submit recorded work to the GPU and advance to the submitted state.
    pub fn submit(self) -> Result<SubmittedFrame<'surface>, Error> {
        Error::check(unsafe { sys::optics_gfx_frame_submit(self.raw) })?;
        Ok(SubmittedFrame {
            raw: self.raw,
            _surface: PhantomData,
        })
    }

    /// Frame-in-flight slot index. Resources written by the GPU should keep
    /// one instance per slot when the previous frame may still be executing.
    pub fn index(&self) -> u32 {
        unsafe { sys::optics_gfx_frame_index(self.raw) }
    }

    /// Swapchain image acquired for this frame, stable until present.
    ///
    /// Damage history and any content-preservation cache belong to this index,
    /// not to [`Self::index`]: the presentation engine may own more images than
    /// there are frames in flight, and the mapping from slot to image is not
    /// stable across frames. For offscreen surfaces this equals the slot.
    pub fn image_index(&self) -> u32 {
        unsafe { sys::optics_gfx_frame_image_index(self.raw) }
    }

    /// The exact pixels this recording frame must repaint, in physical surface
    /// pixels. `Damage::Full` means repaint the whole extent; `Damage::None`
    /// means the acquired image is already current (a capture/pacing frame).
    ///
    /// The scope is gfx's own answer, derived from the per-image epoch history
    /// and the damage passed to the begin call. Callers must not maintain a
    /// parallel per-image damage ledger.
    pub fn repaint_scope(&self) -> Result<Damage, Error> {
        let mut raw = [sys::optics_gfx_rect {
            x: 0.0,
            y: 0.0,
            w: 0.0,
            h: 0.0,
        }; sys::OPTICS_GFX_DAMAGE_CAPACITY as usize];
        let mut count = 0u32;
        let mut full = false;
        let opts = sys::optics_gfx_frame_repaint_scope_opts {
            out_rects: raw.as_mut_ptr(),
            capacity: raw.len() as u32,
            out_count: &mut count,
            out_full: &mut full,
        };
        Error::check(unsafe { sys::optics_gfx_frame_repaint_scope(self.raw, &opts) })?;
        Ok(Damage::from_raw(&raw, count, full))
    }

    /// Current extent of the surface this frame records into.
    pub fn surface_size(&self) -> (u32, u32) {
        self.surface.size()
    }

    /// Begin a depth-tested scene pass targeting this frame's surface.
    ///
    /// `depth` must match the surface extent. The target's previous contents
    /// are discarded and depth is cleared to 1.0. The returned guard ends the
    /// pass on drop; scene drawing APIs accept the guard directly, preventing
    /// them from escaping the pass bracket.
    pub fn begin_scene_pass<'frame>(
        &'frame mut self,
        depth: &'frame Target,
        color: SceneColorLoad,
    ) -> Result<ScenePass<'frame, 'surface>, Error> {
        let (width, height) = self.surface.size();
        if depth.size() != (width, height) {
            return Err(Error(
                sys::optics_gfx_result::OPTICS_GFX_ERROR_INVALID_ARGUMENT,
            ));
        }

        self.begin_scene_pass_raw(
            std::ptr::null_mut(),
            self.surface.format(),
            (width, height),
            depth,
            color,
        )
    }

    /// Begin a depth-tested scene pass targeting a sampleable offscreen image.
    ///
    /// The color image and depth target must have the same extent. The image
    /// is transitioned from sampleable to color-attachment layout for the
    /// pass and restored when the returned guard ends, so Canvas and image
    /// effects can consume it immediately afterward.
    pub fn begin_image_scene_pass<'frame>(
        &'frame mut self,
        target: &'frame Image,
        depth: &'frame Target,
        color: SceneColorLoad,
    ) -> Result<ScenePass<'frame, 'surface>, Error> {
        let size = target.size();
        if depth.size() != size {
            return Err(Error(
                sys::optics_gfx_result::OPTICS_GFX_ERROR_INVALID_ARGUMENT,
            ));
        }
        Error::check(unsafe { sys::optics_gfx_frame_prepare_image_target(self.raw, target.raw) })?;
        self.begin_scene_pass_raw(target.raw, target.format(), size, depth, color)
    }

    fn begin_scene_pass_raw<'frame>(
        &'frame mut self,
        target: *mut sys::optics_gfx_image,
        color_format: Format,
        (width, height): (u32, u32),
        depth: &'frame Target,
        color: SceneColorLoad,
    ) -> Result<ScenePass<'frame, 'surface>, Error> {
        let (load_op, clear_color) = match color {
            SceneColorLoad::Load => (
                sys::optics_gfx_load_op::OPTICS_GFX_LOAD_LOAD,
                [0.0, 0.0, 0.0, 0.0],
            ),
            SceneColorLoad::Clear(rgba) => (sys::optics_gfx_load_op::OPTICS_GFX_LOAD_CLEAR, rgba),
        };
        let color_attachment = sys::optics_gfx_pass_attachment {
            view: if target.is_null() {
                std::ptr::null_mut()
            } else {
                unsafe { sys::optics_gfx_image_vk_image_view(target) }
            },
            format: unsafe { sys::optics_gfx_format_to_vk(color_format.raw()) },
            load_op,
            store_op: sys::optics_gfx_store_op::OPTICS_GFX_STORE_STORE,
            clear_color: vec4(clear_color),
            ..Default::default()
        };
        let depth_attachment = sys::optics_gfx_pass_depth_attachment {
            view: unsafe { sys::optics_gfx_target_vk_view(depth.raw) },
            format: unsafe { sys::optics_gfx_format_to_vk(depth.format.raw()) },
            load_op: sys::optics_gfx_load_op::OPTICS_GFX_LOAD_CLEAR,
            store_op: sys::optics_gfx_store_op::OPTICS_GFX_STORE_DONT_CARE,
            clear_depth: 1.0,
            ..Default::default()
        };
        let pass = sys::optics_gfx_pass_desc {
            type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_PASS_DESC,
            color_attachment_count: 1,
            color_attachments: &color_attachment,
            depth: &depth_attachment,
            width,
            height,
            ..Default::default()
        };
        let vp = sys::optics_gfx_frame_set_viewport_opts {
            x: 0.0,
            y: 0.0,
            width: width as f32,
            height: height as f32,
            min_depth: 0.0,
            max_depth: 1.0,
        };
        let sc = sys::optics_gfx_frame_set_scissor_opts {
            x: 0,
            y: 0,
            width,
            height,
        };
        unsafe {
            sys::optics_gfx_frame_prepare_target(self.raw, depth.raw);
            sys::optics_gfx_frame_begin_pass(self.raw, &pass);
            sys::optics_gfx_frame_set_viewport(self.raw, &vp);
            sys::optics_gfx_frame_set_scissor(self.raw, &sc);
        }
        Ok(ScenePass {
            frame: self,
            active: true,
            target,
        })
    }

    /// The underlying raw `optics_gfx_frame` pointer. Borrowed; the `Frame`
    /// retains ownership. Sibling binding crates (e.g. material) use it to
    /// record material effects into this frame through the C API.
    pub fn as_raw(&self) -> *mut sys::optics_gfx_frame {
        self.raw
    }
}
