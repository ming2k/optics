---
id: ADR-0166
title: "One-Way Library Graph and Enforced Invariants"
status: accepted
date: 2026-11-15
scope: libs/window, libs/app, libs/gfx/meson.build, tools/check_api.py, tests/ui/test_zero_alloc.c, docs/dev/standards
superseded_by: null
negative_knowledge: true
---

# ADR-0166: One-Way Library Graph and Enforced Invariants

- Status: Accepted
- Date: 2026-11-15
- Amends [ADR-0165](0165-single-opts-descriptor-migration-by-library.md) (the
  descriptor rule), [ADR-0024](0024-lens-foundations.md) (the zero-allocation
  wording) and [ADR-0043](0043-iris-foundations.md) (iris no longer depends on
  lens).

## Context

A review of the tree against its own standards found four places where the
documents described a system the code did not implement.

1. **`iris` depended on `lens`.** `iris/app.h` and `iris/event.h` included
   `lens/lens.h`, and each of the three platform backends made the same ~60
   lens calls. Nobody could open a window and draw with flux without linking
   the UI engine, HarfBuzz, FreeType, FriBidi and the SVG compiler, and the
   lens binding existed three times.
2. **flux had four module switches** (`canvas`, `scene`, `compute`, `effect`)
   that produced different exported symbol sets under one SONAME, and the
   "layer" diagram named edges that do not exist.
3. **"Every public entry point takes exactly one descriptor"** was true of 6 of
   92 functions in `flux/core.h` and 39 of 140 in `lens.h`. The lint only
   banned suffixes.
4. **"The frame walk never allocates"** had no test. Writing one showed the
   claim was false as worded: node birth and death and node-store resizing use
   the heap inside `lens_end`.

## Decision

1. **iris depends on flux only.** The UI layer plugs into the frame loop
   through `iris_ui_host` (`libs/iris/include/iris/host.h`): folded input in, a
   frame report out, immutable snapshots for presentation, and deliveries for
   paste, drop, scale, colour scheme and accessibility preferences. iris owns
   its own input and accessibility vocabulary (`IRIS_KEY_*`, `iris_role`, ...).
   Backends reach the host only through `libs/iris/src/ui_host.h`.
2. **`aperture` binds lens to that seam** (`libs/aperture`), once, in portable
   C. Vocabulary agreement between iris and lens is a set of `static_assert`s
   in `aperture.c`, the one translation unit that sees both. `aperture_app_run`
   replaces the lens-flavoured `iris_app_run`; `iris_app_run` remains for
   canvas-only windows and other UI layers.
3. **flux is one library with one ABI.** The four module switches are removed.
4. **The descriptor rule is narrowed and enforced**: at most four parameters,
   at most one descriptor, no variant suffixes, value kernels exempt
   (`docs/dev/standards/api-design.md`). `tools/check_api.py` counts real
   parameters; `tools/config/api_baseline.txt` listed the twelve functions that
   predated it and is now empty — the CPU-canvas and image constructor variants
   were folded into `flux_canvas_desc` / `flux_image_desc`, and the lens
   pattern widgets take their arrays through their options struct.
5. **The allocation invariant is restated as what holds and is tested**: a
   steady-state frame makes zero heap calls
   (`tests/lens/test_zero_alloc.c`, allocator interposition over 600 frames
   under interaction). Structural change may allocate.
6. **Status claims are checkable** (`docs/dev/standards/adr.md`): "landed"
   means a non-test caller uses it; "retired" means deleted. Applying that
   rule to the 2D rasteriser found strokes and non-source-over fills aliased
   on the device since the MSAA default was retired; they now take the
   analytic coverage route, which computes exact pixel area and is explicitly
   bounded. The tile-compute pipeline is recorded as scaffolding, not as the
   device backend.
7. **There are no numbered layers.** The library graph in
   `docs/dev/standards/architecture.md` is the model.

## Alternatives Considered

- **Keep iris above lens and add a second, lens-free window library.** Two
  window stacks to keep in step across three platforms. Rejected.
- **Put the lens binding in lens** (a `lens_iris.c`). Makes the headless UI
  engine depend on a windowing library — the same inversion, mirrored.
- **Share lens's enums with iris through a common header library.** A third
  library that both must agree on, for a few dozen constants. The
  `static_assert` table in aperture gives the same guarantee without a new
  dependency for either side.
- **Convert every function to a descriptor**, as ADR-0165 intended. Yields
  `flux_canvas_translate(c, &(opts){tx, ty})` beside `flux_canvas_rotate(c, r)`:
  ceremony for operands. The value of a descriptor is absorbing configuration
  growth; the rule now says so.
- **Make the frame walk allocation-free unconditionally** by pre-sizing the
  node store and pooling node state. Possible, but it trades a bounded,
  amortised cost on structural change for a fixed memory ceiling. Not needed
  for the property that matters — no heap traffic while the UI is steady.
- **Split flux into `flux-core` and `flux-canvas`.** Deferred: it is a larger
  change than removing the switches, and one ABI already fixes the defect.

## Consequences

- `libiris` links `libflux` and the platform libraries, nothing else.
  `examples/iris/canvas_window` is the proof; `tests/iris` links no lens.
- The lens binding is testable without a compositor
  (`tests/aperture/test_host_binding.c`), which covers the shell on the two
  platforms whose windowed paths CI cannot run.
- Breaking: applications call `aperture_app_run` with `aperture_app_opts`;
  `iris_start_fn` / `iris_stop_fn` lose their `lens *` parameter; the Rust
  `iris` crate loses `Application` and `Config` to the new `aperture` crate.
- High-contrast handling lives in aperture and now restores the scheme the
  theme was following rather than re-querying the system, so a forced-dark
  application stays dark.
- A canvas-only host (`iris_app_opts.ui == nullptr`) is a supported
  configuration on every backend.
- Not verified by this change: the Cocoa backend was converted by the same
  mechanical substitution as Wayland and Win32 but could not be compiled on
  the development machine. The Win32 backend cross-compiles; the Wayland
  backend builds, passes its tests and runs.
