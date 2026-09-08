# ADR-0085: lens borrowed-string registry (debug borrow checker)

## Status

Accepted (implemented: libs/lens/src/core/borrow.c,
tests/lens/test_borrow_check.c)

## Context

lens widgets take caller-owned strings by borrow — `box.id`, `label`,
`placeholder` — and the API contract is "stable through the frame"
(see lens_widget_content in lens.h). The library stores the pointers;
it never copies them.

The misuse is classic C: a caller passes a stack temporary.

```c
// WRONG — dangling on the next frame
char name[32];
snprintf(name, sizeof name, "row%d", row);
lens_label(ui, &(lens_label_opts){.box = {.id = name}, .text = name});
```

Frame N works. Frame N+1 reads freed stack: wrong ids (wrong
animations, wrong hover state, wrong input routing), garbage text, and
— under ASan — a clean stack-use-after-return report. Nothing in the
library could catch it at the misuse site; the crash surfaces frames
later, far from the bug.

Alternatives considered and rejected:

- **Copy every string into the frame arena.** Kills the borrow API:
  every widget pays an allocation and a memcpy, hot per-frame paths
  included, and callers lose the zero-cost static-string pattern that
  makes retained UI cheap. The contract exists precisely because the
  common case is static or frame-stable storage.
- **Valgrind/ASan only.** Catches the dereference, not the cause, and
  only when a build with sanitizers is running — not in the user's
  normal debug build.
- **Fuzz it.** The harness controls the caller; a fuzzer cannot model
  "this pointer was stack memory two frames ago".

## Decision

The shipped lens library always compiles a **borrowed-string
registry** (`LENS_DEBUG_BORROWS` defined in its meson.build; cost: one
bounded array walk per label — nanoseconds against a widget build that
already hashes, measures, and lays out).

Mechanism:

1. `lensi_gen_widget_id` — the single funnel all 16 label/id call
   sites share — registers the caller's pointer in a bounded
   frame-local ring (`LENSI_BORROW_SET_MAX`, 4096; wraps, weakening
   the net without ever misfiring it).
2. Arena-copied strings (semantics names, wrapped lines, preedit
   display) are exempt by construction: any pointer inside the frame
   arena is library-owned for the frame.
3. `lensi_borrow_check(ptr, what)` asserts the pointer is registered
   this frame or arena-owned. On violation it prints the pointer, the
   likely cause, and the fix, then aborts — in debug and release
   alike. A build that must not abort can define `LENS_NO_BORROW_CHECK`
   and rebuild.

The semantics mirror Rust's borrow checker at the granularity that
matters here: not "who owns it" but "is this pointer still pointing at
live storage this frame".

## Consequences

- The misuse is caught at the frame boundary with a named diagnostic,
  instead of as a use-after-return three frames later.
- Zero API change, zero release-build flag juggling: the checker ships
  on. `LENS_NO_BORROW_CHECK` exists for embedded abort-free builds.
- False positives are structurally impossible *for the documented
  contract* (stable through the frame): every legitimate pointer is
  either registered by its widget or copied into the arena. The only
  way to trip it is to actually violate the contract.
- test_borrow_check pins both directions: legitimate patterns must
  not fire; a deregistered pointer must SIGABRT (fork + waitpid on the
  signal).
- Future work if frames ever exceed the ring capacity in practice:
  raise `LENSI_BORROW_SET_MAX`; the ring's wrap degrades detection,
  never correctness.
