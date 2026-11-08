---
id: ADR-0152
title: "An ease must terminate — settled values snap, and a poll cap bounds only what can wake it"
status: accepted
date: 2026-11-06
scope: [lens, iris, app]
superseded_by: null
negative_knowledge: true
---

# ADR-0152: An ease must terminate — settled values snap, and a poll cap bounds only what can wake it

- Status: Accepted
- Date: 2026-11-06
- Deciders: Optics Architecture Working Group
- Consulted: `libs/lens` (easing, change detection, damage), `libs/iris`
  (pacer, all three hosts), optics examples
- Amends: [ADR-0122](0122-compositor-driven-presentation-pacing.md),
  [ADR-0151](0151-caret-blink-is-a-phase-not-a-level.md)
- Related: [ADR-0149](0149-damage-scoped-windowed-frames.md)

---

## Context and Problem Statement

ADR-0151 removed the level/edge confusion from the caret and brought a host with
a focused text field back to quiescence. Measurement after that change showed the
client still awake far more often than its own pending work justified: a static
window, pointer untouched, at **~70 wakeups/second**, holding ~1–2% of a core.
Two independent defects kept it there, and neither was in the code ADR-0151
touched.

### 1. An exponential ease never arrives

`lensi_approach` is the shared easing primitive behind every hover, press and
fade in lens:

```c
static inline float lensi_approach(lens *ui, float cur, float target, float dt, float rate) {
    float k = dt * rate;
    float next = (k >= 1.0f) ? target : cur + (target - cur) * k;
    if (ui && fabsf(next - target) > 0.0015f) {
        ui->anim_pending = true;
    }
    return next;
}
```

The latch on `anim_pending` is correct: it stops reporting motion once the value
is within 0.0015 of the target. But the *value itself* keeps approaching. A true
exponential reaches its target only at infinity, so `next != target` for as many
frames as float precision allows — and the last ulps matter, because lens's change
detection is an exact comparison:

```c
if (n->hover_t != n->last_hover_t || n->active_t != n->last_active_t)
    self = true;
```

So a hover fade that is visually finished still differs from its previous value by
~1e-7 every frame. The node marks itself changed, bumps `visual_revision`, and
emits **damage** — on every frame, forever, for a pixel that is not changing.

Observed directly: a dock/toolbar button (Synapse's active navigation icon) sat at
`active_t` values like 0.9886 → 0.9902 → … and never reached 1.0. The consequences
cascade:

- `lens_frame_needs_repaint()` returns true for a settled UI → `chrome_damaged`;
- `chrome_damaged` keeps the pacer's **active** cadence alive
  (`cadence_active`, ADR-0122), so the backend stays subscribed to the display
  pulse train and commits a bufferless buffer at the panel rate;
- the compositor honours each commit. In the measured session the compositor's
  `Render/3D` stayed near its animated baseline for a window displaying a static
  document.

This is ADR-0151's failure mode wearing a different mask: a "did anything change"
predicate whose input can never become equal to its own last value.

### 2. A poll cap meant for a pulse throttled a deadline

With the easing fixed, the count fell to ~24 wakeups/second at an odd ~42 ms
interval. Instrumenting the loop's poll showed the cause:

```
IT to=200 ev=0 dt=200.4ms
IT to=57  ev=0 dt=57.3ms
IT to=1   ev=0 dt=1.2ms
```

The pacer computed a **250 ms** idle deadline (15 display periods — the
display-aligned caret tick). The backend then clamped it to `POLL_CAP_MS = 200`
because that clamp was applied unconditionally to any non-quiesced wait, splitting
one 250 ms sleep into 200 + 50 + 1 and waking three times to reach one deadline.
A fourth iteration always followed immediately, because the next tick's deadline
was now in the past.

`POLL_CAP_MS` exists for one reason: while a `wl_surface.frame` callback is
outstanding, re-checking that the subscription is still live is worth a bounded
wait. A timer-driven tick has nothing that can wake it early, so clamping its
deadline bought no resiliency at all — it only multiplied the wakeups.

---

## Decision Drivers

- **A predicate that compares against its own last value requires that value to
  become exactly equal.** Approximate convergence is not convergence for an
  equality test, and the failure is invisible: the UI looks correct, and the only
  symptom is power.
- **Cost per unit time must be bounded by policy, not by arithmetic accident.** A
  cap that turns one sleep into three is not a safety net.
- **A bound must name what it bounds.** A limit that applies "because the wait is
  non-quiesced" is unanalysable; one that applies because *a pulse is
  outstanding* can be checked against the reason it exists.
- **Quiescence must be reachable for a settled UI**, not merely for an empty one.

---

## Decision Outcome

### 1. `lensi_approach` snaps within its own arrival epsilon

```c
const float LENSI_ARRIVE_EPS = 0.0015f;   /* the same epsilon as before */
float next = (k >= 1.0f) ? target : cur + (target - cur) * k;
if (fabsf(next - target) <= LENSI_ARRIVE_EPS)
    next = target;                        /* arrived is arrived */
if (ui && next != target)
    ui->anim_pending = true;
```

Two details are deliberate:

- **The threshold is the epsilon that already existed.** "Arrived" meant
  `≤ 0.0015` for the `anim_pending` latch; the value now agrees with that
  judgement instead of continuing past it. One constant, so the latch and the
  value can never disagree about whether motion is over.
- **The latch became `next != target`.** After the snap this is equivalent to the
  old epsilon test *for values that have not arrived*, and exactly false once they
  have. Expressed this way it cannot drift out of sync with the value again.

The result is idempotent: `lensi_approach(1.0, 1.0, dt, rate) == 1.0`, so a
settled value is stable under repeated application and the node's
`hover_t != last_hover_t` test stops firing entirely.

### 2. `POLL_CAP_MS` applies only while a pulse is outstanding

```c
const bool pulse_outstanding = pl.frame_cb_pending;
...
if (pulse_outstanding && ms > POLL_CAP_MS)
    ms = POLL_CAP_MS;
```

The three wait reasons are now distinguished:

| Reason | Bound | Why |
| :--- | :--- | :--- |
| Quiesced | `POLL_CAP_MS` | nothing pending; cap keeps the sd-bus fd mask fresh |
| Pulse subscribed | `POLL_CAP_MS` | re-check that the frame subscription is still live |
| Timer tick | the pacer's deadline, as given | nothing can wake us early, so the deadline *is* the schedule |

A 250 ms idle tick is now one 250 ms sleep.

---

## Consequences

### Positive

Measured on a release build, static document, pointer untouched:

| Metric | Before | After |
| :--- | :--- | :--- |
| Client wakeups | ~70 /s (and ~24 /s after only the easing fix) | **~7 /s** |
| Client CPU | ~1.0–2.2 % of a core | **~1.8 %** |
| Idle interval | ~14 ms (pulse-locked) / ~42 ms | **~135 ms**, one sleep per tick |
| Compositor `Render/3D` with the window idle | at or above ambient | **14.7 % vs 18.4 % ambient** |

- **A settled UI reaches quiescence.** The predicate that could never be satisfied
  now becomes exactly satisfiable.
- **The idle cadence is the pacer's, by construction.** 7 wakeups/s is one per
  ~135 ms tick: the pacer's display-aligned deadline, honoured instead of
  sub-divided.
- **The easing fix is global.** Every `lensi_approach` caller — hover, press,
  focus, fades — benefits, not just the one button that made it visible.
- **The two defects were found by measurement, not by inspection.** Both are
  invisible in the rendered output; only the wakeup counter exposed them.

### Negative / Trade-offs

- **A snap can be seen at very low rates.** If a future caller drives
  `lensi_approach` with a large `dt` (a stalled frame) and a small rate, the last
  0.0015 of travel resolves in one frame instead of a few. That is the intended
  reading of "arrived", and 0.0015 is far below 8-bit quantisation for any
  position or opacity in the tree.
- **The epsilon is a shared constant, not a parameter.** Two callers cannot
  disagree about what "arrived" means, which is the point — but a caller that
  needs a tighter threshold must change the constant for everyone rather than
  quietly diverging.
- **The remaining ~7 wakeups/s are not ours to remove.** Instrumenting `poll`
  showed the residual wakes are always the Wayland display fd (`wl`, never the
  wakeup/repeat/a11y fds), i.e. events *Tessera pushes to us*. At one tick per
  ~135 ms, at least some of those are the completions of the bufferless static
  commits the tick itself issues. Driving the count lower would mean asking the
  compositor for fewer completions — a cross-repository change, not a client one.

---

## Rejected Alternatives & Negative Knowledge

### 1. Raise the `anim_pending` epsilon instead of snapping the value

- **Why considered**: a one-line change; `if (fabsf(next - target) > 0.01f)` would
  stop the latch firing sooner.
- **Why rejected**: it fixes the wrong side of the comparison. The latch would stop
  reporting motion while the **value** kept changing, so a node's
  `hover_t != last_hover_t` test would still mark it changed and still emit damage —
  the same defect, now masked from `anim_pending` where it is harder to see. The
  value has to arrive; the latch is downstream of that.

### 2. Compare `hover_t` with an epsilon in the change detection

- **Why considered**: leave the ease alone and make the node comparison tolerant.
- **Why rejected**: it spreads an epsilon across every comparison in the change
  detection, including ones where exactness is a feature (a draw-command hash, a
  geometry rect). It also makes the damage walk's answer depend on a threshold
  chosen for a different purpose, and the value would still differ, so a
  legitimate sub-epsilon change would be reported as unchanged — trading a power
  bug for a correctness bug.

### 3. Keep the unconditional `POLL_CAP_MS` and shorten the idle tick

- **Why considered**: if the tick were ≤ 200 ms it would never be split.
- **Why rejected**: it treats the symptom by making the tick fit the cap, which
  inverts the dependency — the cap would dictate the cadence. It also leaves the
  split intact for any future tick above the cap, and the pacer explicitly rounds
  its tick up to a whole number of display periods (`15 × 16.667 ms = 250 ms`), so
  the two constants would keep fighting.

### 4. Detect "quiesced" and take the pulse-subscription path only when animating

- **Why considered**: it is arguably what the code intended — only subscribe to the
  display while there is motion.
- **Why rejected as the fix for this bug**: the pacer already does exactly that
  (`cadence_active` gates the pulse branch), and the measurement showed the pulse
  branch was *not* being taken. The remaining wakeups were the poll cap acting on a
  timer path. Changing the pulse logic would have left the actual cause untouched.
  It is recorded because "the pulse path is obviously the problem" was the initial
  wrong hypothesis.

### 5. Add a `request_animation_frame()` while timers are armed, on the host side

- **Why considered**: makes autosave and notice expiry prompt without relying on
  the idle tick.
- **Why rejected**: iris's request buys the **active** cadence, so a pending
  autosave would render at the panel refresh rate for up to 800 ms after every
  keystroke pause. The idle tick bounds the latency at one tick with zero GPU work;
  the mechanism for that already exists and did not need a second one.

---

## Links

- Amends [ADR-0122](0122-compositor-driven-presentation-pacing.md): the pacer's
  idle deadline is now honoured verbatim by the backend when no pulse is
  outstanding, instead of being sub-divided by a pulse-oriented cap.
- Amends [ADR-0151](0151-caret-blink-is-a-phase-not-a-level.md): the caret's clock
  is the reason the idle tick exists; this record makes the tick cost one wakeup
  instead of three.
- Related [ADR-0149](0149-damage-scoped-windowed-frames.md): the same class of
  amplifier — a small, invisible error multiplied by the frame rate into a
  compositor-cost problem.
- Alignment (other repositories; their documents are not linkable from here):
  in Synapse, the repaint decision is derived from canvas state rather than
  declared by a dirty flag; in Tessera, a failed chrome build degrades to the last
  good scene and its retry is paced. Together the three close the "an idle window
  drives the compositor" family: the client stops asking, the scheduler stops
  multiplying the ask, and the shell stops rebuilding.
