# Scene snapshots and presentation

Build UI state with `optics_ui_begin` and `optics_ui_end`, then publish an owned snapshot.
A snapshot contains captured visuals, interaction geometry, overlay ordering and
accessibility strings. It remains readable and renderable after its UI context
is destroyed, subject to the captured GPU resources' device requirements.

```c
optics_ui_begin(ui, &input);
build_ui(ui);
optics_ui_end(ui);

optics_ui_scene_snapshot *snapshot = nullptr;
optics_gfx_result result = optics_ui_snapshot_create(ui, &snapshot);
if (result != OPTICS_GFX_OK)
    return result;

/* The host opens its Canvas session and owns frame submission/presentation. */
result = optics_ui_snapshot_submit(snapshot, canvas);
/* Close the Canvas session, submit the frame, and present it. */
if (result == OPTICS_GFX_OK && successfully_presented)
    result = optics_ui_snapshot_activate(ui, snapshot);
optics_ui_snapshot_release(snapshot);
```

The host must check Canvas close, frame submission and presentation results
before setting `successfully_presented`. A failed or dropped presentation leaves
the previous interaction and accessibility snapshot active. Compilation alone
does not acknowledge damage. A headless host explicitly activates the snapshot
whose geometry it wants inputs to use.

Activation validates the source UI identity and generation. An older generation
cannot replace a newer active generation. Removed or reincarnated nodes do not
inherit stale snapshot interaction state. Accessibility can be read from the
active UI snapshot with `optics_ui_accessibility_walk`, or directly from a retained
snapshot with `optics_ui_snapshot_accessibility_walk`.

Publication is fallible and returns an empty output on failure. Publish only
after `optics_ui_end`, without concurrent UI mutation. All fallible text and child
recording operations propagate their failures; callers must not treat a missing
subtree as a successful frame.

In Rust, `Ui::snapshot()` returns an owning `Snapshot`. The host submits it through
its Canvas session and calls `Ui::activate(&snapshot)` after successful
presentation. The raw Canvas submission method is unsafe because the caller
must uphold Canvas state, target exclusivity and device requirements.

There is no live-tree render call or borrowed draw-list compatibility path.
Frame-scoped node opacity (`optics_ui_set_opacity`) is modulated directly into each
node's compiled draw commands using premultiplied sRGB channel scaling
([ADR-0109](../adr/0109-premultiplied-command-opacity-scissor-bounded-layers-and-glyph-run-batching.md)),
so animated panel and HUD fades execute inside the single active Canvas pass
without spawning per-node offscreen render passes.
Remaining resource-version, execution-plan and specialized-input milestones are
tracked in internal developer implementation records.
