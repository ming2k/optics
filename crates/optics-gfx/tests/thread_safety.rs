//! Static and runtime assertions for the crate's thread-safety contracts
//! (mirroring docs/reference/thread-safety.md).

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

#[test]
fn test_thread_safety_invariants() {
    assert_send::<optics_gfx::Device>();
    assert_sync::<optics_gfx::Device>();

    assert_send::<optics_gfx::Sampler>();
    assert_sync::<optics_gfx::Sampler>();

    assert_send::<optics_gfx::IccProfile>();
    assert_sync::<optics_gfx::IccProfile>();

    assert_send::<optics_gfx::DisplayList>();
    assert_sync::<optics_gfx::DisplayList>();

    assert_send::<optics_gfx::Readback>();
}
