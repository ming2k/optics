# ADR-0095: Flux public contract convergence

- Status: Accepted
- Date: 2026-09-16
- Scope: Flux C API, Rust bindings, and in-tree consumers.
- Supersedes: ADR-0087. Refines ADR-0091 and ADR-0093.

## Context

Several individually useful migrations left incompatible public dialects:
paint and brush, five pass-opening operations, borrowed targets masquerading
as refcounted owners, immediate versus deferred pipeline release, and Rust
sessions that exposed the owning Canvas's lifecycle methods. Some shape/brush
combinations silently substituted other geometry. Such behavior prevents a
caller from deriving correctness from the declared interface.

## Decision

1. Geometry, stroke topology, and brush are separate values. Public drawing
   uses geometry and brush; geometry carries a stroke style. Internal pipeline
   paint is private. Unsupported combinations return an explicit sticky error;
   no geometry substitution, parameter loss, or invalid-value coercion occurs.
2. `flux_canvas_begin(canvas, pass_desc)` and checked `flux_canvas_end(canvas)`
   are the only pass bracket. The descriptor supplies explicit GPU frame context,
   a tagged destination, and pass policy. Default destinations are the Canvas's
   CPU framebuffer or the GPU frame attachment. Image and owned-target
   destinations are explicit alternatives. GPU Canvas remains surface-bound;
   a target is storage, not an implicit command-recording context.
3. Owned targets always retain/release normally. Frames are borrowed execution
   contexts, not refcounted target handles. CPU destination extent and pixels
   determine rendering and load semantics; switching targets never loads an
   unrelated framebuffer. Destination resources are retained through pass close.
4. Pipeline and material release retire GPU objects through their owning device.
   There is no second deferred-release API. Callers keep resources alive through
   command submission; retirement covers submitted GPU work, not arbitrary
   future submission of externally recorded command buffers.
5. Rust sessions exclusively borrow Canvas and destination. They expose only a
   scoped drawing capability, never the Canvas lifecycle. Explicit close returns
   errors. Drop closes recording without submitting. External drawing capabilities
   require an unsafe constructor with an explicit lifetime contract.
6. Immutable display lists own their transitive command payloads. Backend draw
   caches are named and documented separately; they are disposable optimizations,
   not portable content descriptions.
7. This is an intentional source/ABI break. In-tree consumers and documentation
   move together. No deprecated symbols, forwarding compatibility families, or
   legacy ABI assertions remain for the replaced contracts.

## Alternatives Considered

- **Keep aliases during migration.** Preserves the ambiguity and permits new
  consumers to choose the incomplete interface; rejected for this clean break.
- **Pretend all brush/backend combinations work.** Makes a small API misleading.
  Explicit supported semantics and deterministic failure are required instead.
- **Treat a target as both image storage and execution context.** Cannot describe
  image rendering without hidden frame state; these responsibilities are separate.
- **Expose Canvas through a session's Deref.** Permits protocol bypass through
  shared-reference methods; a drawing-only capability is required.

## Consequences

Consumers must rebuild and update their call sites. CPU and Vulkan share the
pass protocol but have explicit capability differences. C retains runtime state
validation; Rust prevents a defined set of invalid safe programs. Neither shared
ownership nor lexical lifetimes claim to prove arbitrary Vulkan synchronization.

Validation must cover stroke style preservation, unsupported combinations,
invalid values, destination switching/loading/extent, pass failure recovery,
resource retirement, and compile-fail session ownership cases.
