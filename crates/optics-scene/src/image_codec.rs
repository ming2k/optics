//! Optional system `libpng` + `libturbojpeg` PNG/JPEG decoders for GLB textures.

use crate::DecodedImage;

#[cfg(not(target_os = "windows"))]
mod sys_codecs {
    use std::ffi::{CStr, c_char, c_int, c_uchar, c_void};
    use std::ptr;

    use crate::DecodedImage;

    const PNG_IMAGE_VERSION: u32 = 1;
    const PNG_FORMAT_RGBA: u32 = 0x02 | 0x01; // PNG_FORMAT_FLAG_COLOR | PNG_FORMAT_FLAG_ALPHA
    const TJPF_RGBA: c_int = 7;

    #[repr(C)]
    struct PngImage {
        opaque: *mut c_void,
        version: u32,
        width: u32,
        height: u32,
        format: u32,
        flags: u32,
        colormap_entries: u32,
        warning_or_error: u32,
        message: [c_char; 64],
    }

    impl Default for PngImage {
        fn default() -> Self {
            Self {
                opaque: ptr::null_mut(),
                version: PNG_IMAGE_VERSION,
                width: 0,
                height: 0,
                format: 0,
                flags: 0,
                colormap_entries: 0,
                warning_or_error: 0,
                message: [0; 64],
            }
        }
    }

    impl Drop for PngImage {
        fn drop(&mut self) {
            unsafe { png_image_free(self) };
        }
    }

    unsafe extern "C" {
        fn png_image_begin_read_from_memory(
            image: *mut PngImage,
            memory: *const c_void,
            size: usize,
        ) -> c_int;
        fn png_image_finish_read(
            image: *mut PngImage,
            background: *const c_void,
            buffer: *mut c_void,
            row_stride: i32,
            colormap: *mut c_void,
        ) -> c_int;
        fn png_image_free(image: *mut PngImage);

        fn tjInitDecompress() -> *mut c_void;
        fn tjDestroy(handle: *mut c_void) -> c_int;
        fn tjDecompressHeader3(
            handle: *mut c_void,
            jpeg_buf: *const c_uchar,
            jpeg_size: std::ffi::c_ulong,
            width: *mut c_int,
            height: *mut c_int,
            jpeg_subsamp: *mut c_int,
            jpeg_colorspace: *mut c_int,
        ) -> c_int;
        fn tjDecompress2(
            handle: *mut c_void,
            jpeg_buf: *const c_uchar,
            jpeg_size: std::ffi::c_ulong,
            dst_buf: *mut c_uchar,
            width: c_int,
            pitch: c_int,
            height: c_int,
            pixel_format: c_int,
            flags: c_int,
        ) -> c_int;
        fn tjGetErrorStr2(handle: *mut c_void) -> *const c_char;
    }

    pub(super) fn decode_png_rgba(
        bytes: &[u8],
        max_edge: u32,
        max_alloc: u64,
    ) -> Result<DecodedImage, String> {
        let mut img = PngImage::default();
        let ok = unsafe {
            png_image_begin_read_from_memory(&mut img, bytes.as_ptr().cast(), bytes.len())
        };
        if ok == 0 || (img.warning_or_error & 0x03) > 1 {
            return Err(png_error(&img));
        }
        if img.width == 0 || img.height == 0 || img.width > max_edge || img.height > max_edge {
            return Err(format!(
                "PNG dimensions {}x{} exceed limit {max_edge}",
                img.width, img.height
            ));
        }
        let byte_len = u64::from(img.width)
            .checked_mul(u64::from(img.height))
            .and_then(|px| px.checked_mul(4))
            .filter(|len| *len <= max_alloc)
            .ok_or_else(|| "PNG decoded buffer exceeds allocation limit".to_string())?;

        img.format = PNG_FORMAT_RGBA;
        let mut rgba = vec![0u8; byte_len as usize];
        let ok = unsafe {
            png_image_finish_read(
                &mut img,
                ptr::null(),
                rgba.as_mut_ptr().cast(),
                0,
                ptr::null_mut(),
            )
        };
        if ok == 0 || (img.warning_or_error & 0x03) > 1 {
            return Err(png_error(&img));
        }
        Ok(DecodedImage {
            width: img.width,
            height: img.height,
            rgba,
        })
    }

    pub(super) fn decode_jpeg_rgba(
        bytes: &[u8],
        max_edge: u32,
        max_alloc: u64,
    ) -> Result<DecodedImage, String> {
        struct TjHandle(*mut c_void);
        impl Drop for TjHandle {
            fn drop(&mut self) {
                if !self.0.is_null() {
                    unsafe { tjDestroy(self.0) };
                }
            }
        }

        let handle = TjHandle(unsafe { tjInitDecompress() });
        if handle.0.is_null() {
            return Err("tjInitDecompress failed".into());
        }
        let mut width = 0 as c_int;
        let mut height = 0 as c_int;
        let mut subsamp = 0 as c_int;
        let mut colorspace = 0 as c_int;
        let rc = unsafe {
            tjDecompressHeader3(
                handle.0,
                bytes.as_ptr(),
                bytes.len() as std::ffi::c_ulong,
                &mut width,
                &mut height,
                &mut subsamp,
                &mut colorspace,
            )
        };
        if rc != 0 {
            return Err(tj_error(handle.0));
        }
        if width <= 0 || height <= 0 || width as u32 > max_edge || height as u32 > max_edge {
            return Err(format!(
                "JPEG dimensions {width}x{height} exceed limit {max_edge}"
            ));
        }
        let w = width as u32;
        let h = height as u32;
        let byte_len = u64::from(w)
            .checked_mul(u64::from(h))
            .and_then(|px| px.checked_mul(4))
            .filter(|len| *len <= max_alloc)
            .ok_or_else(|| "JPEG decoded buffer exceeds allocation limit".to_string())?;

        let mut rgba = vec![0u8; byte_len as usize];
        let rc = unsafe {
            tjDecompress2(
                handle.0,
                bytes.as_ptr(),
                bytes.len() as std::ffi::c_ulong,
                rgba.as_mut_ptr(),
                width,
                0,
                height,
                TJPF_RGBA,
                0,
            )
        };
        if rc != 0 {
            return Err(tj_error(handle.0));
        }
        Ok(DecodedImage {
            width: w,
            height: h,
            rgba,
        })
    }

    fn png_error(img: &PngImage) -> String {
        let msg = unsafe { CStr::from_ptr(img.message.as_ptr()) }
            .to_string_lossy()
            .into_owned();
        if msg.is_empty() {
            "libpng decode failed".into()
        } else {
            msg
        }
    }

    fn tj_error(handle: *mut c_void) -> String {
        let ptr = unsafe { tjGetErrorStr2(handle) };
        if ptr.is_null() {
            "libturbojpeg decode failed".into()
        } else {
            unsafe { CStr::from_ptr(ptr) }
                .to_string_lossy()
                .into_owned()
        }
    }
}

pub(crate) fn decode_image_rgba(mime_type: &str, bytes: &[u8]) -> Result<DecodedImage, String> {
    #[cfg(not(target_os = "windows"))]
    {
        match mime_type {
            "image/png" => sys_codecs::decode_png_rgba(
                bytes,
                crate::materials::MAX_TEXTURE_EDGE,
                crate::materials::MAX_IMAGE_ALLOCATION,
            ),
            "image/jpeg" => sys_codecs::decode_jpeg_rgba(
                bytes,
                crate::materials::MAX_TEXTURE_EDGE,
                crate::materials::MAX_IMAGE_ALLOCATION,
            ),
            other => Err(format!("unsupported base-colour image MIME type {other}")),
        }
    }

    #[cfg(target_os = "windows")]
    {
        let _ = (mime_type, bytes);
        Err(
            "built-in system image codecs are not available on Windows; use \
             Scene::from_glb_with_materials_and_decoder with a caller-supplied decoder"
                .into(),
        )
    }
}

#[cfg(all(test, not(target_os = "windows")))]
mod tests {
    use super::decode_image_rgba;

    // Valid 1x1 red RGBA PNG (70 bytes).
    const PNG_1X1_RED: &[u8] = &[
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, // PNG signature
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, // IHDR len + type
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, // 1x1
        0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, // 8-bit RGBA + CRC
        0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, // IDAT len + type
        0x78, 0x9C, 0x63, 0xF8, 0xCF, 0xC0, 0xF0, 0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, // zlib
        0x89, 0x99, 0x3D, 0x1D, // IDAT CRC
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, // IEND
        0xAE, 0x42, 0x60, 0x82, // IEND CRC
    ];

    #[test]
    fn decodes_minimal_png() {
        let img = decode_image_rgba("image/png", PNG_1X1_RED).expect("1x1 PNG must decode");
        assert_eq!(img.width, 1);
        assert_eq!(img.height, 1);
        assert_eq!(img.rgba, vec![255, 0, 0, 255]);
    }

    #[test]
    fn rejects_invalid_png_and_jpeg() {
        assert!(decode_image_rgba("image/png", b"not a png").is_err());
        assert!(decode_image_rgba("image/jpeg", b"not a jpeg").is_err());
        assert!(decode_image_rgba("image/webp", b"RIFF").is_err());
    }
}
