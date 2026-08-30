# ADR-0086: Cross-Platform Drag-and-Drop Subsystem

- Status: Accepted
- Date: 2026-08-14
- Scope: iris (L3 toolkit), lens (L2 UI engine), flux/anim (visual feedback & springs)

## Context

Cross-application and intra-application Drag-and-Drop (DnD) is an essential desktop interaction model spanning file imports/exports, tab reordering, canvas item rearrangement, and text snippet dragging.

Prior to this ADR:
1. Iris only wired passive drop-target receipt on Wayland via `wl_data_device` (`ddev_enter`, `ddev_motion`, `ddev_drop`), which decoded incoming text/URIs and forwarded them directly to `lens_paste()`.
2. Win32 and Cocoa backends did not expose drop targets.
3. No Drag-Source capability (`IRIS_CAP_DRAG_SOURCE`) existed on any backend; applications could not initiate outgoing cross-process drags.
4. Lens lacked an immediate-mode DnD state machine to declare drag handles, specify drag payloads, render drag ghosts, and hit-test drop zones with visual insertion indicators.

## Decision

We introduce a full-stack, architectural Drag-and-Drop subsystem across Iris and Lens with the following responsibilities:

### 1. Iris (L3 Platform Layer) — `<iris/dnd.h>`

Iris owns the platform protocol integration, MIME content negotiation, asynchronous pipe/stream I/O, and platform drag lifecycle:

- **Actions Bitmask (`iris_dnd_action`)**:
  - `IRIS_DND_ACTION_NONE = 0`
  - `IRIS_DND_ACTION_COPY = 1 << 0`
  - `IRIS_DND_ACTION_MOVE = 1 << 1`
  - `IRIS_DND_ACTION_LINK = 1 << 2`
  - `IRIS_DND_ACTION_ASK  = 1 << 3`

- **Drag Source API**:
  - `iris_dnd_start()` initiates an outgoing drag session with supported MIME types, action masks, and asynchronous data provider callbacks.
  - On Wayland: creates `wl_data_source`, registers listeners (`send`, `cancelled`, `dnd_drop_performed`, `dnd_finished`, `action`), and invokes `wl_data_device_start_drag()`.
  - On Win32/Cocoa: graceful degradation paths conforming to the feature discovery contract (`iris_supports(IRIS_CAP_DRAG_SOURCE)`).

- **Drop Target API**:
  - Notifies active drop target coordinates, negotiated MIME payload, and action matches.
  - Automatically handles stream reading on a detached worker thread with timeout guards, stripping trailing CRLF for URI-lists.

### 2. Capability Discovery — `<iris/capability.h>`

- Adds `IRIS_CAP_DRAG_SOURCE = 10` to the compile-time capability table alongside `IRIS_CAP_DROP_TARGET = 7`.

### 3. Lens (L2 Immediate-Mode UI Layer) — `<lens/lens.h>`

Lens provides declarative immediate-mode APIs for both drag initiation and drop reception:

- **Drag Source**:
  - `lens_drag_source_begin()` / `lens_drag_source_end()`: Tracks pointer dragging beyond hysteresis distance (default 4px), manages active drag payload, and signals host to initiate platform drag.
- **Drop Target**:
  - `lens_drop_target_begin()` / `lens_drop_target_end()`: Hit-tests current pointer position against declared drop zone bounding box, matches action mask, and yields `lens_drop_info`.
- **Payload Access**:
  - `lens_drop_get_text()` / `lens_drop_get_uris()` for consuming dropped content during the drop frame.

### 4. Visual Feedback & Physics

- When a drag begins, Lens/Iris manages the drag preview ghost.
- When an outgoing drag is dropped or cancelled, `anim_spring` drives a snap-back animation if desired.

## Consequences

- **Atomic Capabilities**: Feature availability is discoverable via `iris_supports(IRIS_CAP_DRAG_SOURCE)` and `iris_supports(IRIS_CAP_DROP_TARGET)`.
- **Asynchronous Safety**: Data pipe I/O runs on worker threads; callbacks deliver safely onto the Iris main thread queue.
- **Zero Alloc in Frame Loops**: Immediate-mode DnD uses arena memory and persistent state slots without heap thrashing.
