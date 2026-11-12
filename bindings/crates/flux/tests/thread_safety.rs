//! Static and runtime assertions for the crate's thread-safety contracts
//! (mirroring docs/reference/thread-safety.md).

fn assert_send<T: Send>() {}
fn assert_sync<T: Sync>() {}

#[test]
fn test_thread_safety_invariants() {
    assert_send::<flux::Device>();
    assert_sync::<flux::Device>();

    assert_send::<flux::Sampler>();
    assert_sync::<flux::Sampler>();

    assert_send::<flux::Material>();
    assert_sync::<flux::Material>();

    assert_send::<flux::IccProfile>();
    assert_sync::<flux::IccProfile>();

    assert_send::<flux::DisplayList>();
    assert_sync::<flux::DisplayList>();

    assert_send::<flux::Readback>();
}
