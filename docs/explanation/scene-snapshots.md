# Scene snapshots and presentation

Build UI state with `lens_begin` and `lens_end`, then publish an owned snapshot.
A snapshot contains captured visuals, interaction geometry, overlay ordering and
accessibility strings. It remains readable and renderable after its UI context
is destroyed, subject to the captured GPU resources' device requirements.

```c
lens_begin(ui, &input);
build_ui(ui);
lens_end(ui);

lens_scene_snapshot *snapshot = nullptr;
flux_result result = lens_snapshot_create(ui, &snapshot);
if (result != FLUX_OK)
    return result;

/* The host opens its Canvas session and owns frame submission/presentation. */
result = lens_snapshot_submit(snapshot, canvas);
/* Close the Canvas session, submit the frame, and present it. */
if (result == FLUX_OK && successfully_presented)
    result = lens_snapshot_activate(ui, snapshot);
lens_snapshot_release(snapshot);
```

The host must check Canvas close, frame submission and presentation results
before setting `successfully_presented`. A failed or dropped presentation leaves
the previous interaction and accessibility snapshot active. Compilation alone
does not acknowledge damage. A headless host explicitly activates the snapshot
whose geometry it wants inputs to use.

Activation validates the source UI identity and generation. An older generation
cannot replace a newer active generation. Removed or reincarnated nodes do not
inherit stale snapshot interaction state. Accessibility can be read from the
active UI snapshot with `lens_accessibility_walk`, or directly from a retained
snapshot with `lens_snapshot_accessibility_walk`.

Publication is fallible and returns an empty output on failure. Publish only
after `lens_end`, without concurrent UI mutation. All fallible text and child
recording operations propagate their failures; callers must not treat a missing
subtree as a successful frame.

In Rust, `Ui::snapshot()` returns an owning `Snapshot`. The host submits it through
its Canvas session and calls `Ui::activate(&snapshot)` after successful
presentation. The raw Canvas submission method is unsafe because the caller
must uphold Canvas state, target exclusivity and device requirements.

There is no `lens_render` or borrowed `lens_draw_list` compatibility path.
See the [implementation matrix](../dev/architecture-implementation-status.md)
for the remaining resource-version, execution-plan and specialized-input work.
