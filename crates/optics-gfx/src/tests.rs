use super::*;

#[test]
fn encoder_records_dynamic_slot_and_exposes_budget() {
    let mut encoder = Encoder::new().unwrap();
    encoder.draw_image_slot(0.0, 0.0, 8.0, 8.0, 3, ImageStyle::default());
    let list = encoder.finish().unwrap();
    assert_eq!(list.input_mask(), 1u64 << 3);
    assert_eq!(list.retained_bytes(), 0);
    assert_eq!(list.command_count(), 1);
}

#[test]
fn frame_begin_desc_maps_timeout_and_non_blocking() {
    let desc_default = sys::optics_gfx_frame_begin_desc {
        type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_FRAME_BEGIN_DESC,
        timeout_ns: 0,
        non_blocking: false,
        ..unsafe { std::mem::zeroed() }
    };
    assert_eq!(desc_default.timeout_ns, 0);
    assert!(!desc_default.non_blocking);

    let desc_nb = sys::optics_gfx_frame_begin_desc {
        type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_FRAME_BEGIN_DESC,
        timeout_ns: 0,
        non_blocking: true,
        ..unsafe { std::mem::zeroed() }
    };
    assert!(desc_nb.non_blocking);

    let desc_timeout = sys::optics_gfx_frame_begin_desc {
        type_: sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_FRAME_BEGIN_DESC,
        timeout_ns: 16_666_667,
        non_blocking: false,
        ..unsafe { std::mem::zeroed() }
    };
    assert_eq!(desc_timeout.timeout_ns, 16_666_667);
    assert!(!desc_timeout.non_blocking);
}

#[test]
fn shadow_params_map_to_descriptor_fields() {
    let params = ShadowParams {
        blur: 8.0,
        offset_x: 3.0,
        offset_y: 5.0,
        tint_red: 0.1,
        tint_green: 0.2,
        tint_blue: 0.3,
        alpha: 0.5,
    };
    let raw = std::mem::MaybeUninit::<sys::optics_gfx_image>::zeroed();
    // SAFETY: optics_gfx_image is an opaque type; a zeroed value is never
    // dereferenced, only its address is carried in the descriptor.
    let image = unsafe { raw.assume_init() };
    let image_ref = Image {
        raw: &image as *const _ as *mut _,
    };
    let desc = params.into_desc(&image_ref);
    assert_eq!(
        desc.type_,
        sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_EFFECT_SHADOW_DESC
    );
    assert!(desc.next.is_null());
    assert_eq!(desc.blur, 8.0);
    assert_eq!(desc.offset_x, 3.0);
    assert_eq!(desc.offset_y, 5.0);
    assert_eq!(desc.tint_red, 0.1);
    assert_eq!(desc.tint_green, 0.2);
    assert_eq!(desc.tint_blue, 0.3);
    assert_eq!(desc.alpha, 0.5);
    assert_eq!(
        core::mem::size_of::<sys::optics_gfx_effect_shadow_desc>(),
        56
    );
}

#[test]
fn drm_node_maps_to_strict_device_extension() {
    let raw = DrmNode {
        major: 226,
        minor: 128,
    }
    .raw();
    assert_eq!(
        raw.type_,
        sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_DEVICE_DRM_NODE_DESC
    );
    assert!(raw.next.is_null());
    assert_eq!(raw.drm_major, 226);
    assert_eq!(raw.drm_minor, 128);
}

#[test]
fn semantic_device_features_compose_without_vulkan_names() {
    let features = DeviceFeatures::DMABUF | DeviceFeatures::DMABUF_SYNC_FILE;
    assert!(features.contains(DeviceFeatures::DMABUF));
    assert!(features.contains(DeviceFeatures::DMABUF_SYNC_FILE));
    assert!(!DeviceFeatures::NONE.contains(DeviceFeatures::DMABUF));
}

#[test]
fn image_data_len_validates_supported_formats() {
    assert_eq!(image_data_len(3, 2, Format::R8Unorm).unwrap(), 6);
    assert_eq!(image_data_len(3, 2, Format::Rgba8Unorm).unwrap(), 24);
    assert_eq!(image_data_len(3, 2, Format::Rgba16Sfloat).unwrap(), 48);
    assert_eq!(
        image_data_len(1, 1, Format::Rgb10a2Unorm).unwrap_err(),
        Error(sys::optics_gfx_result::OPTICS_GFX_ERROR_UNSUPPORTED)
    );
}

#[test]
fn readback_region_byte_len_is_checked() {
    assert_eq!(
        ReadbackRegion {
            x: 0,
            y: 0,
            width: 0,
            height: 7,
        }
        .byte_len(),
        None
    );
    assert_eq!(
        ReadbackRegion {
            x: 17,
            y: 9,
            width: 12,
            height: 7,
        }
        .byte_len(),
        Some(12 * 7 * 4)
    );
    assert_eq!(
        ReadbackRegion {
            x: 0,
            y: 0,
            width: u32::MAX,
            height: u32::MAX,
        }
        .byte_len(),
        None
    );
}

/* -------------------------------------------------------------- */
/*  Color management (ADR-0069/0070)                               */
/* -------------------------------------------------------------- */

#[test]
fn color_space_presets_follow_c_semantics() {
    // scRGB names the extended-range intent over the same
    // {primaries, transfer} as sRGB-linear (optics_gfx_color_space_equal).
    assert_eq!(ColorSpace::SCRGB, ColorSpace::SRGB_LINEAR);
    assert_eq!(ColorSpace::SRGB, ColorSpace::SRGB);
    assert_ne!(ColorSpace::SRGB, ColorSpace::SRGB_LINEAR);
    assert_ne!(ColorSpace::DISPLAY_P3, ColorSpace::SRGB);
    assert_ne!(ColorSpace::DISPLAY_P3, ColorSpace::DISPLAY_P3_LINEAR);
    assert_ne!(ColorSpace::BT2020_PQ, ColorSpace::BT2020_HLG);

    assert_eq!(ColorSpace::SRGB.primaries(), ColorPrimaries::Bt709);
    assert_eq!(ColorSpace::SRGB.transfer(), TransferFunction::Srgb);
    assert_eq!(ColorSpace::BT2020.gamma(), 2.4);
    assert_eq!(ColorSpace::ADOBE_RGB.gamma(), 563.0 / 256.0);
    assert_eq!(ColorSpace::ADOBE_RGB.transfer(), TransferFunction::Gamma);

    for space in [
        ColorSpace::SRGB,
        ColorSpace::SRGB_LINEAR,
        ColorSpace::SCRGB,
        ColorSpace::DISPLAY_P3,
        ColorSpace::DISPLAY_P3_LINEAR,
        ColorSpace::ADOBE_RGB,
        ColorSpace::BT2020,
        ColorSpace::BT2020_PQ,
        ColorSpace::BT2020_HLG,
    ] {
        assert!(space.is_valid(), "{space:?} should be valid");
    }

    // A Gamma transfer without a positive exponent is invalid, as is
    // a custom space without chromaticities.
    assert!(!ColorSpace::new(ColorPrimaries::Bt709, TransferFunction::Gamma).is_valid());
    assert!(!ColorSpace::custom(PrimariesXy::default(), TransferFunction::Srgb).is_valid());
    let custom = ColorSpace::custom(
        PrimariesXy {
            rx: 0.64,
            ry: 0.33,
            gx: 0.30,
            gy: 0.60,
            bx: 0.15,
            by: 0.06,
            wx: 0.3127,
            wy: 0.3290,
        },
        TransferFunction::Linear,
    );
    assert!(custom.is_valid());
    // Equality is by primaries tag, not by gamut: a custom space never
    // equals the named BT.709 space even with identical chromaticities.
    assert_ne!(custom, ColorSpace::SRGB_LINEAR);
    assert_eq!(custom, custom);
}

#[test]
fn transfer_functions_round_trip() {
    for tf in [
        TransferFunction::Linear,
        TransferFunction::Srgb,
        TransferFunction::Pq,
        TransferFunction::Hlg,
    ] {
        for v in [0.0f32, 0.001, 0.18, 0.5, 1.0] {
            let back = tf.decode(0.0, tf.encode(0.0, v));
            assert!(
                (back - v).abs() < 1e-4,
                "{tf:?} round trip of {v} gave {back}"
            );
        }
    }
    let gamma = TransferFunction::Gamma;
    let back = gamma.decode(2.2, gamma.encode(2.2, 0.25));
    assert!((back - 0.25).abs() < 1e-5);

    // The ColorSpace helpers use the space's own transfer + gamma.
    let encoded = ColorSpace::SRGB.encode(0.18);
    assert!((ColorSpace::SRGB.decode(encoded) - 0.18).abs() < 1e-5);
    let encoded = ColorSpace::BT2020.encode(0.25);
    assert!((ColorSpace::BT2020.decode(encoded) - 0.25).abs() < 1e-5);
}

#[test]
fn transform_matrix_is_identity_for_same_primaries() {
    // Transfer functions do not enter the matrix: sRGB -> sRGB-linear
    // is a primaries-only no-op.
    let m = ColorSpace::SRGB
        .transform_matrix(ColorSpace::SRGB_LINEAR)
        .expect("both spaces are valid");
    let identity = Mat3::identity().as_array();
    for (a, b) in m.as_array().iter().zip(identity.iter()) {
        assert!((a - b).abs() < 1e-5, "expected identity, got {a} vs {b}");
    }

    // Invalid spaces yield no matrix.
    let invalid = ColorSpace::new(ColorPrimaries::Bt709, TransferFunction::Gamma);
    assert!(invalid.transform_matrix(ColorSpace::SRGB).is_none());
    assert!(ColorSpace::SRGB.transform_matrix(invalid).is_none());

    // Display P3 -> sRGB is a genuine gamut transform, not identity.
    let m = ColorSpace::DISPLAY_P3_LINEAR
        .transform_matrix(ColorSpace::SRGB_LINEAR)
        .unwrap();
    let delta: f32 = m
        .as_array()
        .iter()
        .zip(identity.iter())
        .map(|(a, b)| (a - b).abs())
        .sum();
    assert!(delta > 1e-3, "P3 -> sRGB should differ from identity");
}

#[test]
fn mat3_multiply_transform_invert() {
    let identity = Mat3::identity();
    assert_eq!(identity.multiply(identity), identity);
    assert_eq!(
        identity.transform_vec3([0.5, 0.25, 0.75]),
        [0.5, 0.25, 0.75]
    );

    // A transform and its inverse round-trip a vector.
    let m = ColorSpace::SRGB_LINEAR
        .transform_matrix(ColorSpace::DISPLAY_P3_LINEAR)
        .unwrap();
    let v = [0.5f32, 0.25, 0.75];
    let back = m.invert().transform_vec3(m.transform_vec3(v));
    for (a, b) in back.iter().zip(v.iter()) {
        assert!((a - b).abs() < 1e-4, "invert round trip gave {a} vs {b}");
    }

    // Singular matrices invert to the identity.
    let singular = Mat3::from_raw(sys::optics_gfx_mat3 { m: [0.0; 9] });
    assert_eq!(singular.invert(), Mat3::identity());
}

#[test]
fn surface_hdr_maps_to_raw_extension() {
    let raw = SurfaceHdr {
        sdr_white_nits: 300.0,
        metadata: Some(Hdr10Metadata {
            mastering: PrimariesXy {
                rx: 0.708,
                ry: 0.292,
                gx: 0.170,
                gy: 0.797,
                bx: 0.131,
                by: 0.046,
                wx: 0.3127,
                wy: 0.3290,
            },
            max_luminance: 1000.0,
            min_luminance: 0.01,
            max_cll: 800.0,
            max_fall: 400.0,
        }),
    }
    .raw();
    assert_eq!(
        raw.type_,
        sys::optics_gfx_struct_type::OPTICS_GFX_TYPE_SURFACE_HDR_DESC
    );
    assert!(raw.next.is_null());
    assert_eq!(raw.sdr_white_nits, 300.0);
    assert!(raw.has_metadata);
    assert_eq!(raw.mastering.rx, 0.708);
    assert_eq!(raw.mastering.wy, 0.3290);
    assert_eq!(raw.max_luminance, 1000.0);
    assert_eq!(raw.min_luminance, 0.01);
    assert_eq!(raw.max_cll, 800.0);
    assert_eq!(raw.max_fall, 400.0);

    let bare = SurfaceHdr {
        sdr_white_nits: 0.0,
        metadata: None,
    }
    .raw();
    assert!(!bare.has_metadata);
    assert_eq!(bare.sdr_white_nits, 0.0);
}

/* --------------------------------------------------------------
 * Synthetic ICC profiles, ported from the C test kit
 * (tests/gfx/icc_testkit.h) — same big-endian binary layout.
 * -------------------------------------------------------------- */

fn w_u16(buf: &mut Vec<u8>, v: u16) {
    buf.extend_from_slice(&v.to_be_bytes());
}

fn w_u32(buf: &mut Vec<u8>, v: u32) {
    buf.extend_from_slice(&v.to_be_bytes());
}

fn w_s15f16(buf: &mut Vec<u8>, v: f64) {
    w_u32(buf, (v * 65536.0).round() as i32 as u32);
}

fn w_align(buf: &mut Vec<u8>) {
    while buf.len() % 4 != 0 {
        buf.push(0);
    }
}

/// Tag data block plus its (sig, offset, size) table entries.
struct IccTagBlock {
    data: Vec<u8>,
    defs: Vec<([u8; 4], u32, u32)>,
}

impl IccTagBlock {
    fn new() -> Self {
        Self {
            data: Vec::new(),
            defs: Vec::new(),
        }
    }

    fn begin(&mut self, sig: [u8; 4]) {
        w_align(&mut self.data);
        let offset = self.data.len() as u32;
        self.defs.push((sig, offset, 0));
    }

    fn end(&mut self) {
        w_align(&mut self.data);
        let last = self.defs.last_mut().unwrap();
        last.2 = self.data.len() as u32 - last.1;
    }

    fn xyz(&mut self, sig: [u8; 4], x: f64, y: f64, z: f64) {
        self.begin(sig);
        self.data.extend_from_slice(b"XYZ ");
        w_u32(&mut self.data, 0);
        w_s15f16(&mut self.data, x);
        w_s15f16(&mut self.data, y);
        w_s15f16(&mut self.data, z);
        self.end();
    }

    /// parametricCurveType 4 with the exact sRGB constants.
    fn para_srgb(&mut self, sig: [u8; 4]) {
        self.begin(sig);
        self.data.extend_from_slice(b"para");
        w_u32(&mut self.data, 0);
        w_u16(&mut self.data, 4);
        w_u16(&mut self.data, 0);
        for v in [
            2.4,
            1.0 / 1.055,
            0.055 / 1.055,
            1.0 / 12.92,
            0.04045,
            0.0,
            0.0,
        ] {
            w_s15f16(&mut self.data, v);
        }
        self.end();
    }
}

/// Display-class ("mntr") matrix + sRGB-parametric-TRC profile — the
/// exact flavour the C parser tests extract onto OPTICS_GFX_COLOR_SPACE_SRGB.
fn srgb_icc_profile() -> Vec<u8> {
    // sRGB colorants adapted to the D50 PCS (the published ICC values).
    const SRGB_D50: [f64; 9] = [
        0.4360747, 0.2225045, 0.0139322, // rXYZ
        0.3850649, 0.7168786, 0.0971045, // gXYZ
        0.1430804, 0.0606169, 0.7141733, // bXYZ
    ];
    let mut block = IccTagBlock::new();
    block.xyz(*b"rXYZ", SRGB_D50[0], SRGB_D50[1], SRGB_D50[2]);
    block.xyz(*b"gXYZ", SRGB_D50[3], SRGB_D50[4], SRGB_D50[5]);
    block.xyz(*b"bXYZ", SRGB_D50[6], SRGB_D50[7], SRGB_D50[8]);
    block.para_srgb(*b"rTRC");
    block.para_srgb(*b"gTRC");
    block.para_srgb(*b"bTRC");

    // 128-byte header + tag table, mirroring icc_build_profile.
    let mut h = Vec::new();
    w_u32(&mut h, 0); // size, patched below
    w_u32(&mut h, 0); // CMM
    w_u32(&mut h, 0x0430_0000); // ICC v4.3
    h.extend_from_slice(b"mntr"); // display device class
    h.extend_from_slice(b"RGB "); // data color space
    h.extend_from_slice(b"XYZ "); // PCS
    for v in [2026u16, 8, 15, 12, 0, 0] {
        w_u16(&mut h, v); // creation date/time
    }
    h.extend_from_slice(b"acsp");
    for _ in 0..6 {
        w_u32(&mut h, 0); // platform, flags, manufacturer, model, attributes
    }
    w_u32(&mut h, 0); // rendering intent: perceptual
    for v in [0.9642, 1.0, 0.8251] {
        w_s15f16(&mut h, v); // D50 illuminant
    }
    for _ in 0..12 {
        w_u32(&mut h, 0); // creator, profile ID, reserved
    }
    assert_eq!(h.len(), 128);

    w_u32(&mut h, block.defs.len() as u32);
    let base = (h.len() + block.defs.len() * 12) as u32;
    for (sig, offset, size) in &block.defs {
        h.extend_from_slice(sig);
        w_u32(&mut h, base + offset);
        w_u32(&mut h, *size);
    }
    let total = base + block.data.len() as u32;
    h[0..4].copy_from_slice(&total.to_be_bytes());
    h.extend_from_slice(&block.data);
    h
}

#[test]
fn icc_profile_extracts_synthetic_srgb() {
    let bytes = srgb_icc_profile();

    // Raw sys layer: create/release round trip succeeds.
    let mut raw: *mut sys::optics_gfx_icc_profile = std::ptr::null_mut();
    let rc = unsafe {
        sys::optics_gfx_icc_profile_create(
            bytes.as_ptr() as *const std::os::raw::c_void,
            bytes.len(),
            &mut raw,
        )
    };
    assert_eq!(rc, sys::optics_gfx_result::OPTICS_GFX_OK);
    assert!(!raw.is_null());
    unsafe { sys::optics_gfx_icc_profile_release(raw) };

    // Safe layer: the matrix + sRGB-TRC profile extracts exactly onto
    // ColorSpace::SRGB (same assertion as the C parser tests).
    let profile = IccProfile::new(&bytes).expect("parse synthetic sRGB profile");
    let space = profile.color_space().expect("parametric-extractable");
    assert_eq!(space, ColorSpace::SRGB);
    assert_eq!(space.primaries(), ColorPrimaries::Bt709);
    assert_eq!(space.transfer(), TransferFunction::Srgb);

    // Clone retains; dropping the original leaves the clone usable.
    let cloned = profile.clone();
    drop(profile);
    assert_eq!(cloned.color_space(), Some(ColorSpace::SRGB));
}

#[test]
fn icc_profile_rejects_malformed_input() {
    assert_eq!(
        IccProfile::new(&[]).unwrap_err(),
        Error(sys::optics_gfx_result::OPTICS_GFX_ERROR_INVALID_ARGUMENT)
    );
    assert_eq!(
        IccProfile::new(&[0u8; 64]).unwrap_err(),
        Error(sys::optics_gfx_result::OPTICS_GFX_ERROR_INVALID_ARGUMENT)
    );
    // A valid sRGB profile truncated below its declared size.
    let bytes = srgb_icc_profile();
    assert_eq!(
        IccProfile::new(&bytes[..bytes.len() / 2]).unwrap_err(),
        Error(sys::optics_gfx_result::OPTICS_GFX_ERROR_INVALID_ARGUMENT)
    );
}
