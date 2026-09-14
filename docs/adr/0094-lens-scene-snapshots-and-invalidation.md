# ADR-0094: Lens scene snapshots and explicit invalidation

- Status: Accepted
- Date: 2026-09-14
- Scope: Lens layout, visuals, interaction, accessibility, and caches.
- Depends on: [ADR-0089](0089-rendering-program-and-execution-plan.md),
  [ADR-0090](0090-display-list-resource-ownership.md),
  [ADR-0091](0091-drawing-group-and-effect-semantics.md).
- Implementation: Complete; full scene snapshot compilation without GPU and replay tested.

## Context

Live-tree replay in `libs/lens/src/render/replay.c` mixes mutable UI state
with rendering. Its new compile entry currently emits only root background.
A complete scene boundary must cover text, custom visuals, overlays, leave
animations, interaction geometry, and accessibility as well as layout boxes.

## Decision

Lens updates mutable UI state from explicit events and samples of time,
theme, font environment, and host preferences. Layout and visual compilation
publish a **SceneSnapshot** with immutable owned content and stable node
identities. A snapshot includes placement, transforms, clips, order, visual
content, hit-test information, accessibility semantics, and generation.
Nonvisual metadata need not be duplicated into the DisplayList.

Text measurement uses CPU font services. No device, window, atlas, or GPU
upload is required to build a snapshot. Internal memoization is allowed;
"pure" does not prohibit caches or imply unsynchronized shared font state.
Parallel work operates on immutable inputs with an explicit dependency join
before publication. Arbitrary independent subtree layout is not promised.

Visual compilation consumes only the snapshot and explicit compilation
inputs. It produces the same Flux DisplayList used by non-Lens producers.
Lens does not own a second GPU batcher or effect scheduler. A Lens wrapper
may retain damage and node mapping metadata but cannot duplicate command
counts or introduce another command language with independent semantics.

### Complete visual publication

All visual output, including tooltips, menus, selection, caret, icons,
custom skins, and leaving nodes, enters the snapshot. Custom visual callbacks
run during snapshot construction and capture owned output; GPU replay never
calls application code or reads mutable UI nodes. Animation time is sampled
explicitly rather than read during replay. Group opacity uses ADR-0091.

### Identity and invalidation

Stable node identity is distinct from a snapshot generation. Cache
dependencies include content/resource versions, layout constraints, style
and theme generations, font configuration, sampled animation state, and
the scale or output properties actually baked into the cached representation.
Device-specific output properties belong in plan keys when not baked into
the device-independent snapshot.

Reuse requires matching dependencies, not a "needs repaint" flag alone.
Dirty propagation accounts for old and new bounds, clip changes, reordering,
group composition, and effect sampling footprints. Unknown dependencies
force conservative invalidation. Snapshot retention and cache eviction obey
explicit memory budgets.

Hit testing and accessibility use a published snapshot generation. The
default input geometry is the last successfully presented generation; a
headless host explicitly activates a generation. Failed presentation does
not promote a newer snapshot. Actions resolve stable IDs against live UI
state and reject removed or stale targets. Host accessibility adapters may
run separately but consume versioned data rather than traversing a mutable
render tree. This replaces ADR-0029's implicit previous-frame timing for the
new snapshot path while preserving host-owned event delivery.

## Alternatives Considered

- **Record GPU calls while walking live nodes.** Prevents independent
  replay and makes retained output depend on UI mutation timing.
- **Snapshot only layout rectangles.** Leaves text, effects, and overlays
  reading live state and fails to establish a usable rendering boundary.
- **Give Lens its own optimizer.** Duplicates Flux ordering and target
  capability logic; Lens should optimize semantic reuse and invalidation.

## Consequences

Snapshots add retained memory and require generation-aware host delivery.
Replace `lens_render(ui, canvas)` and the partial compile entry with one
complete publication/compilation path; migrate Iris integration, Rust
bindings, and all custom visual consumers. No old replay adapter remains.
Layout algorithms and widget identity policies remain reusable subject to
the new publication contract.

## Acceptance Criteria

- Build a complete UI snapshot without a graphics device and replay it
  after destroying the mutable UI and its temporary inputs.
- Include text, nested clips, scrolling, overlays, tooltips, custom skins,
  opacity groups, and leave animations in snapshot correctness tests.
- Font, theme, scale, resource updates, and effect-input damage invalidate
  exactly the required caches or conservatively invalidate more.
- Input and accessibility tests cover delayed/failed presentation, removed
  nodes, retained old snapshots, and explicit headless activation.
- Lens contains no private GPU scheduling path or visual replay callback.
