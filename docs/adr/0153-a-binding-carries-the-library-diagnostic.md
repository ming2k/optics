---
id: ADR-0153
title: "A binding must carry the library's diagnostic, not just its result code"
status: accepted
date: 2026-11-06
scope: [bindings, flux, lens]
superseded_by: null
negative_knowledge: true
---

# ADR-0153: A binding must carry the library's diagnostic, not just its result code

- Status: Accepted
- Date: 2026-11-06
- Deciders: Optics Architecture Working Group
- Consulted: `bindings/lens-rs`, `bindings/flux-rs`, `libs/flux` (error slot,
  pass validation), `libs/lens`
- Amends: —
- Related: [ADR-0149](0149-damage-scoped-windowed-frames.md)

---

## Context and Problem Statement

A live session logged this, repeatedly:

```
WARN shell: chrome scene build failed: shell render: Render(FLUX_ERROR_INVALID_ARGUMENT)
```

That message is unactionable, and not because the log line is terse. Flux *did*
record what went wrong: `flux_get_last_error` returns the failing entry point, the
source file and line inside flux, a human-readable detail, and a backend code. The
detail survived all the way to the binding and was then thrown away, here:

```rust
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    Create(sys::flux_result),
    Render(sys::flux_result),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Create(rc) => write!(f, "lens_create failed (flux_result = {rc:?})"),
            Error::Render(rc) => write!(f, "snapshot operation failed (flux_result = {rc:?})"),
        }
    }
}
```

Three things about this shape are wrong, and each one is independently sufficient
to destroy the diagnosis:

1. **`Copy` on the error forces the payload to be a scalar.** `flux_result` is a
   bare enum discriminant. An `ErrorInfo` with owned strings cannot be `Copy`, so
   deriving `Copy` guaranteed that no diagnostic could ever be carried.
2. **The read has to happen at the failure site.** Flux's diagnostic lives in a
   thread-local slot that the *next* failing call overwrites. By the time a caller
   formatted the error, the slot described some later failure, or none. A binding
   that does not read it inside the construct arm can never recover it.
3. **`FLUX_ERROR_INVALID_ARGUMENT` names a class, not a cause.** A non-finite rect,
   an out-of-bounds render area, a duplicate pass extension and a canvas pass begun
   twice all produce it. Each needs a different fix; the code cannot pick between
   them.

The consequence was concrete: the `Render(FLUX_ERROR_INVALID_ARGUMENT)` failure in
Tessera's shell could be contained (a previous record made it degrade to the last
good scene and paced its retry) but not *diagnosed*, because every retry logged the
same four-word class name.

---

## Decision Drivers

- **A diagnostic is only recoverable at the point of failure.** Deferring the read
  is not "later, with more context" — it is "never".
- **An error type's derives are an architectural decision.** `Copy` is not a
  convenience; it constrains what an error may say, and therefore what a support
  engineer can know.
- **The typed form and the rendered form must both exist.** A caller deciding how
  to react needs the code; a human reading a log needs the message. `Display`-only
  forces string matching, struct-only forces every caller to format.
- **Bindings are the boundary where information has historically been dropped**, so
  the contract to preserve it has to be pinned by a test — nothing else notices its
  loss, because the error still looks plausible.

---

## Decision Outcome

### 1. The error carries a `FluxDiagnostic`, captured at construction

```rust
pub enum Error {
    Create(FluxDiagnostic),
    Render(FluxDiagnostic),
}

pub struct FluxDiagnostic {
    pub code: i32,                    // the flux_result discriminant
    pub code_name: String,            // "FLUX_ERROR_INVALID_ARGUMENT"
    pub function: Option<String>,     // "flux_canvas_begin_pass"
    pub file: Option<String>,
    pub line: i32,
    pub message: Option<String>,      // which argument, which bound, which state
    pub backend_code: i32,
}
```

Every construction site goes through one arm:

```rust
pub(crate) fn from_result(ctor: fn(FluxDiagnostic) -> Self, rc: sys::flux_result) -> Self {
    ctor(FluxDiagnostic::read(rc))
}
```

`read` consults `flux::ErrorInfo::last()` **at that moment**, copying the fields out
of the thread-local slot before any other flux call can overwrite it. `Copy` is gone
from `Error`.

### 2. `code_name` is a string, and that is deliberate

`lens_sys` binds the enum from its own build of the C headers, so its
`flux_result` is nominally a distinct type from `flux::sys`'s even though both
mirror the same C enum. Bridging them needs a `TryFrom<i32>` that bindgen does not
emit. Rather than add a conversion whose only job is to satisfy the type checker,
the diagnostic keeps the numeric code (for programmatic decisions) *and* flux's own
`{:?}` rendering (for greppable logs). Nothing is lost, and no lossy cast is
introduced on the path that carries the message.

### 3. `Display` renders the whole diagnostic

```
FLUX_ERROR_INVALID_ARGUMENT at flux_canvas_begin_pass (canvas.c:666): Canvas damage scope names no pixel of the target
```

So a host that only logs `{error}` — as Tessera's shell does, unchanged — gets the
entry point, the location and the cause for free.

### 4. `PartialEq` is structural on code + message

`Error` had `PartialEq` before, and callers may rely on it. It is now implemented
explicitly, comparing the code and the message but **not** the location: two calls
that fail for the same reason at the same entry point are the same error to a
caller deciding how to react, and a source line is a property of flux's build, not
of the failure.

### 5. The contract is pinned by a test

`tests/smoke.rs::a_failed_call_carries_the_flux_diagnostic` drives a real rejection
and asserts the rendered diagnostic names the flux code and that the structured
fields survive. It exists because the regression is invisible: dropping the payload
again would still produce a plausible-looking error, and every test that only
checks "it failed" would keep passing.

---

## Consequences

### Positive

- **The shell's existing failure log became actionable with no change at its call
  site.** It now reports which flux entry point rejected the call and why.
- **Every existing `?` in the bindings improved at once**, because the change is in
  the error type rather than at each site.
- **The typed/rendered split is available**: a caller can match on `code` and a log
  can carry the message.
- **The `Copy` constraint that forbade this is gone**, so a future error variant
  can carry context without redesigning the type.

### Negative / Trade-offs

- **`Error` is no longer `Copy`.** It was `Copy` and `Eq`; it is now `Clone`-free,
  `Eq`-free and heap-carrying. Any binding-internal code that relied on copying an
  error had to change. This was preferable to keeping a scalar payload, which is
  what forced the information loss in the first place.
- **One `String` allocation per failure.** Failures are exceptional by definition,
  and the alternative is not "cheaper" — it is "still cheap because it says
  nothing".
- **`#[doc(hidden)] FluxDiagnostic::for_test` is public.** A test can only pin the
  contract through a public door, and the reconstruction path is otherwise private.
  It is documented as test-only rather than made `pub(crate)`, because an
  integration test lives outside the crate.

---

## Rejected Alternatives & Negative Knowledge

### 1. Keep `Copy` and add a `&'static str` message

- **Why considered**: preserves the existing derives and avoids allocation; a
  `&'static str` could name the entry point.
- **Why rejected**: flux's message is built at the failure site with the offending
  value in it (`"... scope names no pixel of the target"`, with the rect), so it is
  not a static string. Restricting the payload to `&'static str` would carry the
  entry point and lose the cause — half the diagnosis, which is roughly where the
  code started.

### 2. Read the thread-local slot lazily in `Display`

- **Why considered**: zero cost on the success path, and no struct to carry.
- **Why rejected**: by `Display` time the slot is gone. The slot is overwritten by
  the next failing flux call, and there usually *is* a next one — a failed pass is
  typically followed by a failed submit. This is the specific mistake the record
  exists to prevent, so it is spelled out in the `Comment` above: a lazy read is a
  guaranteed-lossy read.

### 3. Have flux store a ring of past diagnostics

- **Why considered**: would make a lazy read viable, and would help every binding.
- **Why rejected**: it changes flux's error contract for every consumer (C, Rust,
  and any future binding) to serve a pattern that is unnecessary once the binding
  reads eagerly at the failure site. The eager read is correct and local; a ring
  buffer is a system-wide widening to solve a problem that does not exist if each
  binding does the right thing.

### 4. Keep two error variants: `Code(flux_result)` and `Diagnostic(Box<Info>)`

- **Why considered**: preserves the cheap scalar path for callers that only compare
  codes, and avoids allocation when no detail was recorded.
- **Why rejected**: it creates two shapes for one thing, so every caller matches
  twice and the `Code` arm is the one a hurried caller writes — reintroducing the
  loss whenever the detail was available. The `no detail recorded` case is already
  representable inside one shape (`function: None`), which is strictly better than a
  variant that discards it.

### 5. Fix the log line in Tessera instead of the binding

- **Why considered**: the symptom appeared in Tessera's log, so the surface fix is
  there.
- **Why rejected**: Tessera cannot read flux's thread-local slot — by the time its
  `log::warn!` runs, several flux calls have happened. The information has to travel
  through the error type or it does not travel at all. Fixing it at the log line
  would have produced a longer message with the same missing content.

---

## Links

- Related [ADR-0149](0149-damage-scoped-windowed-frames.md): the specific failure
  that motivated this record was a rejected damage scope
  (`Canvas damage scope names no pixel of the target`), which is one of the
  `FLUX_ERROR_INVALID_ARGUMENT` causes the code alone could not distinguish.
- Consuming side (other repository, not linkable from here): in Tessera, a failed
  chrome build degrades to the last good scene with a paced retry, and the pass
  scope now travels as the damage set — so this diagnostic arrives on a failure
  path that is already contained and actionable.
