---
id: ADR-0151
title: "A caret blink is a phase, not a level — lens owns the clock, the host forwards the edge"
status: accepted
date: 2026-11-06
scope: [lens, iris, bindings, app]
superseded_by: null
negative_knowledge: true
---

# ADR-0151: A caret blink is a phase, not a level — lens owns the clock, the host forwards the edge

- Status: Accepted
- Date: 2026-11-06
- Deciders: Optics Architecture Working Group
- Consulted: `libs/lens` (repaint query, damage walk, text widgets, skins),
  `libs/iris` (frame pacer, all three hosts), `bindings/lens-rs`,
  `bindings/iris-rs`
- Amends: [ADR-0143](0143-regional-damage-rect-lists-and-paint-bounds-localization.md),
  [ADR-0149](0149-damage-scoped-windowed-frames.md)
- Extends: [ADR-0122](0122-compositor-driven-presentation-pacing.md),
  [ADR-0137](0137-caller-declared-backdrop-layer-revisions.md)

---

## Context and Problem Statement

A note-taking host (`synapse-app`) observed that with a Wayland compositor
(`tessera`) as the session, the compositor's `Render/3D` engine climbed to
**~50% GPU utilisation while the application window sat idle and unfocused by
input** — and the client's own GPU consumption stayed at ~0%. Reducing the
scope to what actually differed between "idle" and "at rest", the trigger was a
2-pixel text caret.

The mechanism was not a rendering inefficiency in any single place. It was a
**type error in the damage contract**, propagated through three layers:

### 1. Lens reported a level where a transition was required

`libs/lens/src/core/context.c` had, in `lens_frame_needs_repaint`:

```c
/* A focused text widget keeps the caret clock alive: the host paces
 * low-frequency frames for the blink, and each of those frames must
 * actually paint. */
if (ui->caret_rect.w > 0.0f)
    return true;
```

`caret_rect.w > 0` is **true for as long as any text widget holds focus**. It is
a *level*: a statement about state, not about change. The query it lived in
answers "would this frame paint something different from what is on screen", so
a level term made that answer permanently `true`.

`damage_resolve` carried the matching defect — it pushed `ui->caret_rect`
unconditionally, so every frame of a focused field named its caret box as
damage.

There was a second, load-bearing consequence: **the caret never actually
blinked.** Lens held no clock at all. The skin painted the caret whenever
`show_caret` was set:

```c
if (c->show_caret) { /* ... push caret rect ... */ }
```

so a "blink" could only ever be produced by a *host* that chose not to paint
some frames. Because the repaint query was permanently true, no host could skip
any frame, and the caret was in practice solid. The two defects masked each
other: the reason every frame repainted was also the reason the caret appeared
not to blink.

### 2. Iris could not express the difference

`iris_pace_inputs` carried a single field:

```c
/* A focused text caret is blinking. */
bool caret_visible;   /* filled from lens_caret_rect(ui).w > 0.0f */
```

This is the same type error one layer up. It conflated two facts that need
opposite handling:

- **a clock is live**, so the pacer must keep a low idle tick running (lens can
  only advance a blink clock on a frame);
- **a flip landed**, so one frame owes a *paint*.

Supplied as a single level, it went straight into `cadence_any`:

```c
bool cadence_any = cadence_active || in->caret_visible || in->has_custom_paint;
```

With a focused text widget this was permanently true, so
`IRIS_PACE_QUIESCE` was **unreachable**. The pacer could never let the loop
block in `poll()`, a low idle tick ran forever, and — because
`lens_frame_needs_repaint` was simultaneously permanently true —
`chrome_damaged` forced a *paint* on every one of those ticks.

### 3. The compositor amplified what it was told

Every painted frame committed a new swapchain buffer at the top of the traffic
that reached the session compositor. On a 3072×1920 @ 120 Hz panel with a
2160×1440 client surface, that is ~3.1 M pixels per commit entering a composite
pass that must sample, colour-convert and blend them.

The compositor is not at fault, and it cannot defend itself: the Wayland
contract makes a commit authoritative. It has no way to know that the new
buffer is pixel-identical to the last one. The measured result was a
compositor at ~50% `Render/3D` for a window displaying a static document.

**This is the architectural point of the record:** a client-side type error in
a damage predicate is not a local inefficiency. It is amplified by refresh rate
and output area into the *compositor's* budget, in a different process, where
the client cannot see it and no local optimisation can recover it.

---

## Decision Drivers

- **Damage is a transition, never a level.** Any predicate that answers "did
  pixels change" must be derived from state *differences*, not from state
  *presence*. This is the invariant the record exists to make structural.
- **One clock, one owner.** A blink is time-driven. Time-driven state needs an
  authoritative clock that advances exactly once per frame, so every consumer
  of the phase within a frame agrees.
- **Quiescence must remain reachable.** A host showing a static document with a
  focused field must be able to reach 0 frames per second. Any term that makes
  that unreachable is a defect regardless of how small its visual footprint is.
- **No host-side blink models.** If the host has to model the blink to decide
  how often to paint, every host re-implements it, and each one is a new place
  for the level/edge confusion to return.
- **Presentation, not build, defines "on screen".** A flip that is built but
  never presented has not reached the display; treating it as delivered would
  let a skipped frame silently drop a blink.

---

## Decision Outcomes

### 1. Lens owns a caret blink clock, advanced by the frame delta

`lens` gains a phase state advanced **once per build**, in `lens_begin`, before
any widget runs:

```c
static void lensi_caret_advance_blink(lens *ui) {
    if (!ui->caret_blink_enabled || !(ui->caret_blink_period_s > 0.0f))
        return;
    float dt = ui->input.dt_seconds;
    if (!(dt > 0.0f) || !isfinite(dt)) dt = 0.0f;
    const float period = ui->caret_blink_period_s;
    if (dt > 2.0f * period) dt = 2.0f * period;   /* bound a nonsense delta */
    ui->caret_phase_s = fmodf(ui->caret_phase_s + dt, period);
    ui->caret_visible = ui->caret_phase_s < period * 0.5f;
}
```

Advancing in `lens_begin` rather than in the text widget is what makes the
phase *coherent within a frame*: the skin's draw, the repaint query and the
damage walk all read one value. A widget that integrated its own delta would
let two text widgets in one frame disagree.

`lens_input.dt_seconds` is already the frame delta the host supplies, and on a
paced host it is the compositor's pulse interval — so the blink is sampled at
the display's cadence for free, with no second clock in the system.

The period is 1.06 s (a 530 ms half-cycle: the cadence X11 blink, Win32,
AppKit and GTK converged on), overridable per context:

```c
LENS_API void lens_set_caret_blink(lens *ui, float period_seconds);
```

`period <= 0` disables the blink. The caret is then solid **and reports no
damage at all**, which is how reduced-motion is expressed: the host owns that
policy, lens executes it.

### 2. The repaint query asks whether the phase *flipped*

```c
static bool lensi_caret_phase_changed(const lens *ui) {
    if (ui->caret_rect.w <= 0.0f || ui->caret_rect.h <= 0.0f)
        return false;
    return ui->caret_visible != ui->caret_presented_visible;
}
```

`lens_frame_needs_repaint` now consults this instead of the rect's existence,
and `damage_resolve` pushes the caret rect only on the same condition. A caret
that exists and has not flipped contributes **no damage**: not a repaint, not a
rectangle.

The comparison is against `caret_presented_visible`, recorded in
`lens_snapshot_activate` from a phase captured into the snapshot at build time.
Comparing against the *presented* phase — not the last built one — is what
makes a skipped frame keep its flip pending. It also means the phase cannot be
derived by a caller from `lens_caret_visible` alone, which is why the
comparison is exported as one function rather than two primitives:

```c
LENS_API bool lens_caret_visible(const lens *ui);       /* current phase   */
LENS_API bool lens_caret_flip_pending(const lens *ui);  /* phase != shown  */
LENS_API bool lens_caret_clock_live(const lens *ui);    /* clock running   */
```

### 3. The two facts are exported separately, because they act differently

`iris_pace_inputs.caret_visible` is **deleted**. It is replaced by:

```c
/* A caret blink clock is live. A level: keeps the low idle tick alive so lens
 * can advance the clock. Never buys active pacing and never forces a paint. */
bool caret_ticking;
```

and the flip itself needs no new field at all: lens already reports it through
`lens_frame_needs_repaint` → `chrome_damaged`, which is the pacer's existing
"this frame changed" edge. So:

- `caret_ticking` → `cadence_any` (keeps the idle tick; the clock can advance);
- `chrome_damaged` on the flip frame → `cadence_active` (that frame paints).

The caret term is **removed from the paint decision entirely**. A blink is a
discrete ~2 Hz phase change, not a continuous animation, and it must never buy
the display-locked cadence.

The skin obeys the phase, so the painted pixels finally match the reported
damage:

```c
if (c->show_caret && ui->caret_visible) { /* push caret rect */ }
```

During an IME preedit the caret is pinned visible: frames are event-driven
then, and a caret that vanished between them reads as a lost insertion point.

### 4. Only a widget-owned caret belongs to lens; a host-owned one does not

Lens exposes two caret setters and they are now genuinely different things:

| API | Producer | Painter | Blink clock | Damage |
| :--- | :--- | :--- | :--- | :--- |
| `lensi_set_caret_rect` (internal) | a lens text widget | lens skin | **lens** (`dt_seconds`) | `chrome_damaged` |
| `lens_set_caret_rect` (public) | a host editing surface | the host's paint callback | **the host** | the host's own paint decision |

The public setter's documented purpose is to place the IME candidate window: an
application that paints its own text has its own caret, its own blink cadence
and its own notion of when to repaint. Lens records the geometry (the platform
layer needs it for `zwp_text_input_v3.set_cursor_rectangle`) and claims **no
ownership**: `caret_lens_owned` stays false, so `lensi_caret_phase_changed`
returns false and `lens_caret_clock_live` returns false for that caret.

Without that distinction lens would advance a phase for a caret it never paints,
and report flips the host's painter did not honour — the original defect
inverted. `caret_lens_owned` is reset every `lens_begin` like the rect it
describes, so a stale claim cannot persist across a frame in which the other
producer took the caret back.

### 5. All three hosts use the same authority

Wayland, Win32 and Cocoa each fill `caret_ticking` from
`lens_caret_clock_live(ui)`. None of them derives a phase, a period, or a
comparison. The comparison lives in exactly one place — and it answers `false`
for a caret the host owns.

---

## Consequences

### Positive

- **Quiescence is reachable again.** With a focused text field and no input, the
  repaint query returns `false`, the pacer skips, and the loop blocks. The
  application stops committing buffers, so the compositor stops compositing.
- **The caret blinks, correctly, for the first time.** The phase is owned by the
  component that can express it as damage, and it is sampled at the display's
  cadence rather than at whatever rate a host happened to paint.
- **Damage is exactly the caret's box.** A blink costs a 2-pixel-wide rectangle,
  not a field repaint and not an output-wide composite.
- **One implementation of blink semantics across three platforms**, because the
  comparison is exported as a single function instead of as two primitives that
  every backend would have to combine itself.
- **`dt_seconds` gets a second, honest consumer.** The frame delta already
  existed for eased values; the blink reuses it instead of adding a clock.

### Negative / Trade-offs

- **A host that never calls `lens_begin` cannot blink.** The clock advances only
  inside a build, which is deliberate (coherence within a frame) but means a
  fully quiescent lens-only host no longer blinks at all. That is the correct
  trade: a caret that cannot be drawn is not worth waking a GPU for, and the
  pacer keeps the idle tick running whenever `caret_ticking` is true, so a
  *live* clock always has frames to advance on.
- **`iris_pace_inputs` is a breaking rename.** There is no forwarder for
  `caret_visible`: keeping it would keep the level/edge confusion alive for any
  host that had not migrated, which is exactly the failure this record removes.
  All three backends and the pacer test are updated in the same change.
- **A period change mid-cycle rewinds the phase** into the new period. A caller
  that shrinks the period can therefore see the caret jump one phase early. This
  is preferable to parking the phase past the new period, which would stall the
  blink permanently.

---

## Rejected Alternatives & Negative Knowledge

### 1. Keep `caret_rect.w > 0` and let the host throttle the frame rate

- **Why considered**: the smallest possible change — leave lens reporting
  "caret exists", and have each host pick a ~4 Hz paint rate for it.
- **Why rejected**: it makes every host re-derive when a caret repaints, and it
  leaves the *damage* wrong while only the *frequency* changes. The compositor
  still receives a commit per tick, and each one still names an unchanged
  surface. It also cannot fix the fact that no component owned the blink, so
  the caret still would not blink. This is the "throttle the symptom" pattern
  that ADR-0122 explicitly rejected for pacing.

### 2. Put the blink clock in iris, and have it toggle the caret itself

- **Why considered**: iris owns the pacer and already has the frame delta, so it
  could compute a phase and hand it to lens.
- **Why rejected**: the host would then be setting a widget's visual state,
  which is lens's job, and the damage walk (which lives in lens) could not see
  the toggle it was supposed to report. It also puts a second clock next to the
  pacer's own, and two clocks in one system drift. The owner of a visual state
  must be the component that can also report its change; otherwise the phase and
  the damage are computed from different facts.

### 3. Emit the caret as an eased value so `anim_pending` carries it

- **Why considered**: lens already has a "still animating" latch that keeps
  hosts painting; reusing it would need no new state.
- **Why rejected**: `anim_pending` is defined as "an eased value is still in
  transit" and is consumed by the pacer as a request for the **active-rate
  cadence**. Wiring a 2 Hz blink into it would make every focused field render at
  the panel refresh rate — precisely the pathology being removed. A discrete
  phase flip is not an animation, and the two must not share a signal.

### 4. Keep `caret_visible` as a deprecated alias for `caret_ticking`

- **Why considered**: avoids touching three backends and the pacer test.
- **Why rejected**: the field name is the bug. `caret_visible` reads as "the
  caret is on screen now", which is a statement about pixels; `caret_ticking`
  reads as "a clock is running", which is a statement about scheduling. An alias
  would let a future reader reintroduce the level-in-the-paint-path defect while
  believing the API still described visibility. A rename with no forwarder is
  the only version that cannot be misused.

### 5. Compare the phase against the last *built* snapshot instead of the presented one

- **Why considered**: cheaper — no field needed on the snapshot, and activation
  is a separate code path.
- **Why rejected**: a build that is not presented must not consume the flip. If
  the comparison used the last built phase, a frame that was skipped after
  building would mark the flip as delivered, and the caret would stay in a phase
  the display never showed — a permanently invisible (or permanently solid)
  caret, depending on which frame won the race. Recording the phase on
  activation from a value captured at build time keeps the two consistent even
  when a snapshot is presented after a newer one was built.

---

## Links

- Amends [ADR-0143](0143-regional-damage-rect-lists-and-paint-bounds-localization.md):
  the caret is now a conditional member of the damage list, not an
  unconditional one.
- Amends [ADR-0149](0149-damage-scoped-windowed-frames.md): the repaint-query
  term that gated a windowed frame's paint is now a transition.
- Extends [ADR-0122](0122-compositor-driven-presentation-pacing.md): the pacer's
  quiescence is reachable again for hosts with a focused text field.
- Extends [ADR-0137](0137-caller-declared-backdrop-layer-revisions.md): same
  class of defect — a predicate derived from mutable state instead of from a
  declared revision edge.
- Alignment: `synapse` ADR-0025 (demand-driven frame pacing) records the
  application-side half of this failure and the compositor measurement.
