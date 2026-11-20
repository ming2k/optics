//! System libpng16 + libturbojpeg PNG/JPEG decoders for GLB textures.

use std::ffi::{CStr, c_char, c_int, c_uchar, c_void};
use std::ptr;

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

pub(crate) fn decode_png_rgba(
    bytes: &[u8],
    max_edge: u32,
    max_alloc: u64,
) -> Result<(u32, u32, Vec<u8>), String> {
    let mut img = PngImage::default();
    let ok =
        unsafe { png_image_begin_read_from_memory(&mut img, bytes.as_ptr().cast(), bytes.len()) };
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
    Ok((img.width, img.height, rgba))
}

pub(crate) fn decode_jpeg_rgba(
    bytes: &[u8],
    max_edge: u32,
    max_alloc: u64,
) -> Result<(u32, u32, Vec<u8>), String> {
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
    Ok((w, h, rgba))
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
