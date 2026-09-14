# ADR-0093: C state contracts and Rust execution sessions

- Status: Accepted
- Date: 2026-09-14
- Scope: C API lifecycle, errors, and Rust bindings.
- Depends on: [ADR-0089](0089-rendering-program-and-execution-plan.md),
  [ADR-0090](0090-display-list-resource-ownership.md),
  [ADR-0092](0092-resource-planning-and-retirement.md).
- Implementation: Complete; sealed AsTarget, CanvasSession target borrow safety, and compile-fail tests verified.

## Context

The current `Canvas::begin(&self, target: &T)` in
`bindings/flux-rs/crates/flux/src/lib.rs` returns no owner of the target
borrow. A lifetime on TargetRef alone cannot constrain the subsequent
execution interval. Uniform retain/release naming also conflates ownership,
mutation permissions, and GPU retirement.

## Decision

Define C contracts first, then make Rust enforce those same contracts.
Opaque handles distinguish owned objects from borrowed access; arbitrary
invalid C pointers remain a caller violation, not a recoverable state error.

| Object | Legal progression |
|--------|-------------------|
| Recorder | Recording -> Finished or Failed; destruction aborts unfinished work. |
| Compiled plan | Immutable validated structure; each execution has separate bindings and leases. |
| Presentation frame | Acquired -> Submitted or Aborted; host presentation consumes submitted ownership through its documented completion path. |
| Execution session | Open -> Closed or Failed; close ends recording, not GPU lifetime. |
| Submission | Pending -> Completed or Failed; retirement follows backend completion obligations. |

Terminal objects cannot be reused without creating new state. Every fallible
creation initializes its output to empty before doing work. A recorder may
use sticky errors: the first error is retained, later recording cannot hide
it, and finish returns failure with no published list. Valid-handle protocol
violations return an explicit error without pretending work succeeded.

An execution session acquires exclusive writable access to its target for
its recording interval. Submission transfers required access leases and
resource retention to in-flight execution. CPU readback cannot expose a
borrowed slice concurrently with writes: it requires completed access and
an exclusive read lease, or returns an owned copy.

### Rust API obligations

Recorder finish consumes the recorder. Session construction borrows mutable
execution state and the target for the session lifetime. Drawing or plan
execution requires that session; no independent method bypasses its borrow.
Frame submission consumes the frame and cannot occur with a live session.
After submission, target reuse is gated by completion, not by the end of a
lexical borrow alone.

Explicit finish/close returns errors. Drop performs abort or cleanup and
must not hide a successful-looking implicit submission. Drop cannot report
an execution result in place of explicit completion handling.

Safe target traits are sealed. Raw-handle imports require unsafe entry
points with ownership, validity, synchronization, and thread obligations.
Send and Sync are justified per type and per reachable state. Shared
reference counting alone is not justification. Borrowed views never release
their owner's reference; owning wrappers release exactly their own share.

Use create/destroy for unique opaque owners, retain/release for shared
owners, and init/deinit for initialized caller storage when appropriate.
Names follow ownership instead of imposing reference counting on every type.
The actual C signatures and Rust types are generated from a single contract
table during implementation; no deprecated API family remains.

## Alternatives Considered

- **Lifetime-marked target plus begin/end on shared Canvas references.**
  Does not keep the target borrowed over the whole session.
- **Only Rust validates states.** Leaves C consumers and FFI failure paths
  with incompatible execution semantics.
- **Reference count every object.** Keeps allocation alive but does not
  establish exclusive mutation or presentation validity.

## Consequences

C and Rust interfaces break together. Runtime C checks remain necessary;
Rust types eliminate a defined subset of invalid safe programs. There is no
claim that lifetimes formally verify backend synchronization or unsafe code.

## Acceptance Criteria

- Compile-fail Rust cases cover frame submission during a session, target
  destruction or mutation during borrowing, overlapping writable sessions,
  forged safe target implementations, and use after consuming finish.
- C tests cover every valid-handle illegal transition and sticky OOM paths.
- Readback and execution cannot create aliased mutable memory in safe Rust.
- Explicit errors, Drop cleanup, external imports, and delayed completion
  neither leak nor release resources twice.
