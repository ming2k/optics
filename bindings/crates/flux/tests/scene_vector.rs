//! Integration test for ADR-0117 vector coverage material in Rust bindings.

use flux::{Device, FillRule, Format, GpuBuffer, Material};

#[repr(C)]
#[derive(Clone, Copy)]
struct Segment {
    x0: f32,
    y0: f32,
    x1: f32,
    y1: f32,
}

#[test]
fn vector_material_creates_and_releases_with_gpu_buffer() {
    let Ok(device) = Device::new(true, &[], &[], 1) else {
        eprintln!("no Vulkan device available; skipping test");
        return;
    };

    let segs = [
        Segment {
            x0: -0.5,
            y0: -0.5,
            x1: 0.5,
            y1: -0.5,
        },
        Segment {
            x0: 0.5,
            y0: -0.5,
            x1: 0.5,
            y1: 0.5,
        },
        Segment {
            x0: 0.5,
            y0: 0.5,
            x1: -0.5,
            y1: 0.5,
        },
        Segment {
            x0: -0.5,
            y0: 0.5,
            x1: -0.5,
            y1: -0.5,
        },
    ];

    let bytes = unsafe {
        std::slice::from_raw_parts(segs.as_ptr() as *const u8, std::mem::size_of_val(&segs))
    };

    let buffer = GpuBuffer::new_storage(&device, bytes).expect("GpuBuffer creation failed");
    assert_ne!(
        buffer.device_address(),
        0,
        "device address must be non-zero"
    );

    let material = Material::new_vector(
        &device,
        [0.2, 0.8, 0.4, 1.0],
        Format::Rgba8Unorm,
        Format::Undefined,
        &buffer,
        4,
        FillRule::NonZero,
    );

    assert!(
        material.is_ok(),
        "Material::new_vector must succeed on device"
    );

    drop(material);
    drop(buffer);
}
