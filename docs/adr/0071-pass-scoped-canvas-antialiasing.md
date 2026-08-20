# ADR-0071: Pass-scoped canvas antialiasing policy

- Status: Accepted
- Date: 2026-08-20
- Supersedes: ADR-0009 (canvas sample count joins the pipeline-cache key)

## Context

ADR-0009 proposed `sample_count` on `flux_canvas_desc` — a canvas-create-time
knob (1/2/4/8) that would flow into the pipeline cache key and allocate a
matching multisample attachment for the canvas's lifetime.

While implementing the color-management pipeline (ADR-0069) and the bucketed
target-attachment pool, two things changed the calculus:

1. **Antialiasing became per-pass, not per-canvas.** Compositor-shaped
   hosts (image blits, video, full-surface uploads) want to render into
   their one-sample destination directly; vector UI on the same canvas in
   a different frame still wants 4×. A create-time constant cannot express
   "this frame is image-heavy, skip the MSAA attachment"; a per-pass policy
   can. This is not a hypothetical: the aphrodite pixel editor's canvas is
   a NEAREST-sampled image blit where a 4× resolve attachment is pure
   waste, while its chrome (rounded rects, marching-ants strokes) wants
   the coverage.
2. **The 4× attachment is allocated per-pass from a pool, not per-canvas
   for a lifetime.** The bucketed target-attachment pool (128px buckets,
   ADR-0020 hardening) already owns sized attachments; a dedicated
   lifetime allocation alongside it would duplicate that machinery.

Meanwhile the part of ADR-0009 that was unambiguously right — sample count
as a pipeline-cache discriminator — landed as an internal two-variant
pipeline axis (`CANVAS_SAMPLE_SINGLE` / `CANVAS_SAMPLE_MSAA`) in
`renderer.c` (commit 229e200), because Vulkan requires pipelines to match
their framebuffer's sample count.

## Decision

`flux_canvas_pass_desc` carries a `flux_canvas_antialias` policy for the
pass being begun:

```c
typedef enum flux_canvas_antialias {
    FLUX_CANVAS_ANTIALIAS_AUTO = 0,     /* clear => 4x MSAA, load => 1x */
    FLUX_CANVAS_ANTIALIAS_NONE = 1,     /* always one-sample */
    FLUX_CANVAS_ANTIALIAS_MSAA_4X = 2,  /* require a clear colour */
} flux_canvas_antialias;
```

- **AUTO** preserves the historical default: a clearing pass uses 4× MSAA
  (the common vector-UI frame), a loading pass renders into its one-sample
  destination (the common incremental frame). Hosts that never think about
  AA keep pre-v0.0.5 behaviour.
- **NONE** always uses the one-sample GPU target — for compositor and
  image-heavy passes that would otherwise pay a 4× allocation and resolve
  for content that gains nothing from coverage samples.
- **MSAA_4X** forces the multisample path and requires `clear_color` (a
  multisample attachment cannot LOAD the contents of its one-sample
  resolve destination; the combination returns an error rather than
  silently degrading).

The sample count itself is fixed at `FLUX_CANVAS_SAMPLES` (4×); 2× and 8×
are not exposed. Nothing shipped wanted them, the resolve cost is
sublinear in sample count at these sizes, and dropping them keeps the
pipeline cache at exactly two variants per (format × paint × stencil ×
blend) instead of four.

The CPU backend accepts the same policy and ignores it — its software
rasterizer has native per-edge coverage.

The legacy `flux_canvas_begin_frame(c, f, clear_color_or_null)` maps onto
`begin_pass` with `AUTO`.

## Consequences

Positive:

- Compositor hosts skip a 4× attachment and resolve pass by setting NONE
  — the case ADR-0009's fixed knob could not express.
- The pipeline cache stays bounded at two sample variants (vs. up to four
  under 0009's 1/2/4/8).
- AUTO keeps every existing host's behaviour without a migration.

Negative:

- Two entry points to know about (`begin_frame` legacy, `begin_pass`
  descriptor form); the descriptor form is where pass shaping lives.
- 2×/8× would need a new enum value and a third/fourth pipeline variant;
  deliberately not taken today.

## Relationship to ADR-0009

0009 diagnosed the pipeline-key requirement correctly (sample count must
discriminate pipelines) and that landed. Its API shape — a canvas-create
`sample_count` of 1|2|4|8 with a lifetime MSAA image — is superseded by
this per-pass policy with a fixed 4×. ADR-0004's "paint kind drives
pipeline selection" is unaffected; the sample variant is one more axis of
the same key, exactly as 0009 argued.
