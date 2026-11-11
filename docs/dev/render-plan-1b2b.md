# RenderPlan 1B-2B — uploads and the presentation blit enter the plan

Status: **landed**. Managed image uploads and the end-of-frame presentation
blit are now `UPLOAD` and `OUTPUT` nodes in the frame's `flux_render_plan`.
`flux_plan` still carries only execution state, and the backend still records
the existing commands. This document is the atomic change boundary for 1B-2B.

## Problem

1B-1 compiled Canvas passes and 1B-2A compiled deferred effects, but two
managed writes still happened outside the only liveness graph: the staging →
image copy in the upload path, and the content → acquired-image copy that makes
a presented frame coherent. A Canvas draw could also sample an image the plan
had never seen a producer for. Those are exactly the hidden writes and reads
ADR-0089/0092 reject.

## Target shape

### Upload node

Every managed image upload performed while a frame is recording on the calling
thread adds one `FLUX_RENDER_NODE_UPLOAD` node:

- WRITE: destination `flux_image`, version 0, full image or the updated
  sub-rect;
- READ: staging buffer identity, version 0 (buffers have no version identity
  yet);
- ordering: after any earlier `UPLOAD` writer of the same destination, and
  before every node that already reads or writes it;
- required capability: `FLUX_RENDER_CAP_TRANSFER`.

Upload submissions are recorded into their own command buffer while the frame
is still recording; the frame's command buffer is submitted afterwards. Every
upload therefore executes before every Canvas/effect node in that plan. The
nodes state that explicitly instead of pretending the upload landed in plan
order. A Canvas pass that itself writes the upload destination is a genuine
conflict (one submission would have to be both before and after it) and fails
validation rather than being guessed. Staging contents in this tree are
produced on the CPU (glyph coverage, icon masks, caller pixels), so there is no
GPU producer node to name. If a GPU-produced staging path is introduced, its
producer must be a plan-owned identity and must be ordered explicitly here; it
must not be inferred from the destination image or accepted as a raw caller
node index. That identity belongs with the submission owner and the execution
switch (1B-3B/1B-4), because a producer edge recorded while the upload still
executes outside the plan command buffer would claim ordering the backend does
not honour. The node is added and the plan validated **before** the copy is
recorded. An upload the plan cannot describe fails instead of becoming a
hidden write. LUT uploads and other internal images with no `flux_image`
identity stay outside the graph; they are never sampled by plan-managed draws.

Uploads outside a frame have no frame plan to join. That is not a missing node:
`flux_surface_begin_frame` flushes the device upload batch before recording, so
such an upload is complete before any frame can sample it. `flux_frame_current`
is a thread-local stack: a worker thread uploading while another thread records
belongs to no frame, and a host recording several surfaces on one thread still
attaches each upload to the right frame.

### Presentation output node

A content-backed windowed frame renders into the persistent content image and
then copies the committed damage into the acquired presentation image. That
copy is a managed write, so it adds one `FLUX_RENDER_NODE_OUTPUT` node:

- READ: the content image (its plan identity is the owning surface);
- WRITE: the acquired presentation image for this frame;
- region: the disjoint damage boxes actually copied, or full when the history
  collapsed the union;
- dependency: the plan's last writer of the content image (the Canvas pass
  that produced it);
- required capability: `FLUX_RENDER_CAP_TRANSFER`.

An empty copy records no node. Offscreen surfaces render directly into their
per-slot image and have no presentation blit.

The Canvas pass's own working-space intermediate and its internal output
transform remain an implementation detail of the Canvas node: the node declares
the destination the pass produces, not every backend barrier on the way there.

### Canvas sampled-image reads

`canvas_track_foreign_image` is the single funnel every sampled image passes
through. When the image already has a producer in this plan (an `UPLOAD`
node, an earlier `CANVAS` target, an `EFFECT` output), the active Canvas node
gains a READ access and a dependency on that producer. External images have no
writer in this plan and need no edge; their upload completed before the frame
began. Access or dependency overflow fails the pass closed rather than
under-declaring the read.

## Validation points

- upload node: immediately after node creation, before the copy is recorded;
- Canvas pass: after the compile gate at begin, again at end after draws have
  appended sampled-image reads;
- presentation blit: immediately after node creation, before the copy is
  recorded.

## Files

- `libs/flux/src/core/render_plan.h/.c` — last-writer lookup and node
  access/dependency append helpers.
- `libs/flux/src/core/plan.c` — `flux_frame_plan_upload`.
- `libs/flux/src/core/frame.c` — current-frame stack, presentation `OUTPUT`
  node, recording-state cleanup.
- `libs/flux/src/core/surface.c` — resize/release leave the current-frame stack
  through the same transition as submit.
- `libs/flux/src/core/oneshot.c` — upload records its node before emitting the
  copy.
- `libs/flux/src/canvas/canvas.c` — Canvas node lifetime and sampled-image
  reads.
- `tests/flux/unit/test_render_plan.c` — upload → canvas → output chains and
  the unordered-read/write rejections.
- `tests/flux/integration/test_render_plan_frame.c` — production wiring:
  create/update during a frame, Canvas draw naming the upload producer.

## Deletion gate

1B-2B is complete only when:

1. every managed upload path that can run during a frame calls
   `flux_frame_plan_upload` before recording its copy;
2. the only writer of the presentation image in a content-backed frame is the
   `OUTPUT` node;
3. no module keeps its own upload/blit last-writer table;
4. `grep` finds no `flux_plan_pass` or separate provider/requires array.

## Exit criteria

- One liveness graph: `flux_render_plan`.
- Upload and presentation-blit writes are explicit nodes with producer edges.
- The existing Canvas/effect execution path is unchanged.
- No new public API.
- ADR-0089 remains `In progress` until 1B-3/4/5 complete.

## Next boundary

**1B-3A**: validate the plan at submit, freeze node order, and make the plan's
budget authoritative in submission.
