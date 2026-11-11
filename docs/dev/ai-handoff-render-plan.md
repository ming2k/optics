# AI Handoff — RenderPlan migration and production hardening

Status: **in progress, uncommitted worktree**. Read this before touching Flux.
Last verified checkpoint: 1B-2B, all 256 tests green.

## Mission

Converge Optics on ADR-0089/0092: one target-specific RenderPlan compiler and
one Submission owner. No second execution path, no second liveness table, no
legacy fallback. This worktree also contains the completed ADR-0090 ownership/
serialization work; do not revert it.

## Already landed

- **ADR-0090 in-process contract**: dynamic image input slots, retained-content
  budget, immutable image snapshots + COW, dma-buf borrowed/exclusive leases,
  ICC/LUT shared color state, snapshot/update serialization, OOM lanes.
- **ADR-0157**: versioned DisplayList wire protocol, serializer/deserializer,
  resource ID callbacks, fuzz harness + corpus, ci-fuzz matrix entry.
- **ADR-0089 1A**: `flux_render_plan` IR, target contract, pure validator
  (`libs/flux/src/core/render_plan.{h,c}`, `tests/flux/unit/test_render_plan.c`).
- **ADR-0089 1B-1**: `flux_canvas_begin` compiles its pass into the frame's
  RenderPlan (target contract, write access, damage region, sequential pass
  dependency) and validates before opening the backend pass.
- **ADR-0089 1B-2A**: deferred effects are `EFFECT` nodes in the same plan;
  `flux_plan` is execution state only (arena, tokens, pool, counters). The
  former `flux_plan_pass` liveness table is deleted.
- **ADR-0089 1B-2B**: managed image uploads that run while a frame records are
  `UPLOAD` nodes (staging READ, destination WRITE, last-writer dependency);
  the content → acquired presentation copy is an `OUTPUT` node; Canvas draws
  append sampled-image READs with producer dependencies. Validation runs
  before each new node's command is emitted. See
  `docs/dev/render-plan-1b2b.md`.

## Key files

- `libs/flux/src/core/render_plan.h/.c` — IR, validator, last-writer lookup,
  node access/dependency append.
- `libs/flux/src/core/plan.h/.c` — deferred-effect execution over RenderPlan,
  `flux_frame_plan_upload`.
- `libs/flux/src/core/frame.c` — frame begin/current-frame stack, presentation
  `OUTPUT` node, submit.
- `libs/flux/src/core/surface.c` — resize/release leave the current-frame stack.
- `libs/flux/src/core/oneshot.c` — upload records its node before the copy.
- `libs/flux/src/canvas/canvas.c` — Canvas pass compile gate, sampled-image
  reads, per-pass node lifetime.
- `libs/flux/src/effect/effect.c` — `flux_frame_register_pass` consumers.
- `libs/flux/src/core/internal.h` — `flux_frame` owns `render_plan` and `plan`.
- `docs/dev/render-plan-1b2a.md` — 1B-2A design/deletion gate.
- `docs/dev/render-plan-1b2b.md` — 1B-2B design/deletion gate.
- `docs/dev/architecture-implementation-status.md` — ADR status matrix.

## Worktree warning

This is an **uncommitted linked worktree** with many intentional changes across
ADR-0090, ADR-0157 and RenderPlan. Do not `git reset`, `git checkout -- .`, or
revert files you did not inspect. `Cargo.lock`/local Optics mode may be
worktree-local; follow the repository hook, do not bypass it.

## Next atomic step: 1B-3A

Validate the plan at submit, freeze node order, and make the budget in
submission authoritative.

Required work:

1. **Validate at submit**
   - run `flux_render_plan_validate` at `flux_frame_submit` before
     `vkEndCommandBuffer`, after the presentation `OUTPUT` node exists;
   - a failed plan must abort the submission with no partial publication.
2. **Freeze order**
   - the plan is immutable once submission starts: reject any node/access/
     dependency append after the freeze point;
   - add an explicit frozen flag (or a state transition) so helpers fail
     closed rather than mutating a plan being consumed.
3. **Budget in submission**
   - compute and enforce transient/in-flight budgets from the frozen plan;
   - report the measured total through the existing diagnostics, not a second
     counter tree.
4. **No execution switch yet**
   - the backend still records the existing commands in 1B-3A. Do not move
     pass opening/execution into the plan until 1B-4.

Acceptance:

- submit validates the exact plan that was recorded;
- no mutation can land after freeze;
- over-budget submission fails before `vkEndCommandBuffer`;
- Canvas, target, effect, material and full integration suites pass;
- no duplicated liveness table or second budget ledger.

## Remaining roadmap after 1B-3A

- **1B-3B**: single Submission owner for in-flight resources/leases; delete
  frame-number retirement guesses. Define the plan-owned producer identity for
  a GPU-produced staging path (do not infer it from the destination, and do not
  accept a raw caller node index).
- **1B-4**: backend opens/executes passes from the plan, not the Canvas call
  stack. Canvas still records commands, plan decides pass boundaries.
- **1B-5**: delete direct Canvas execution, old fallback paths, and the
  independent planner; mark ADR-0089/0092 Landed.
- Then production tracks: retirement/device-loss/OOM pressure, ADR-0093/0094,
  coverage 0116/0119, instrumented fuzz/TSan/soak, bench CI/hardware matrix,
  Rust serialization bindings/cross-platform/YCbCr/release engineering.

## Non-negotiable invariants

- **One liveness graph**: `flux_render_plan`. `flux_plan` is execution state
  only; never add a second pass/provider/requires table.
- **No dual execution**: do not add a “new path” beside the direct Canvas path
  without deleting the old one in the same completion stage.
- **Explicit resource identity**: handle + version + access mode + region.
- **Fail closed**: invalid plan/access/dependency/budget fails before command
  emission; no partial publication or retained resource on failure.
- **No native pointers on the wire**; ADR-0157 is authoritative.
- **No compatibility shims/aliases** in this tree.

## Verification

```bash
meson compile -C build
meson test -C build --no-suite bench     # 256 tests currently green
./tools/gate check --fast              # 8 guardrails
./tools/gate check tidy

# DisplayList wire fuzz. The default `build/` uses the standalone driver:
# one seed file per invocation (or stdin).
./build/tests/flux/fuzz/fuzz_display_list_deserialize \
  tests/flux/fuzz/corpus/display_list_deserialize/bad_flags.bin

# A libFuzzer build (CI fuzz lane) takes the corpus directory:
./build/tests/flux/fuzz/fuzz_display_list_deserialize -runs=0 \
  tests/flux/fuzz/corpus/display_list_deserialize
```

`bench` may fail locally because the Tracy toolchain is absent from `/tmp`;
that is an environment issue, not a code failure. CI installs it.

## Traps already hit

- Effect provider resolution must scan **all** RenderPlan write accesses, not
  only effect outputs: an effect may sample a Canvas target written by an
  earlier Canvas node.
- `flux_plan.pass_count` counts **EFFECT nodes**, but token indexes are
  **RenderPlan node indexes** (Canvas/upload/output nodes interspersed). Never
  use `pass_count` to bound a node index.
- `flux_plan_reset` runs before `flux_render_plan_init` in frame begin; it must
  clear old effect output pending flags from the previous plan.
- Canvas pass target contract uses the surface target; an image attachment is
  the WRITE resource but not a different target contract in 1B-1.
- The serializer must use logical resource keys (`image_key`/`sampler_key`,
  `atlas_key`), not the internal snapshot handles, or caller IDs cannot match.
- The current-frame stack must be left on **every** exit from RECORDING, not
  only submit: resize and surface release abandon a frame without submit.
  MALLOC_PERTURB_ exposes a stale entry immediately as an upload attached to a
  freed frame.
- The presentation `OUTPUT` node uses the surface pointer as the content-image
  identity, matching the Canvas node's default WRITE resource. Do not switch
  one side to a raw `VkImage` handle without switching both.
- Do not add a Canvas node before an upload that the same pass samples:
  dependency direction is reader → producer for sampled images, but an upload
  is submitted *before* the frame command buffer, so a later upload adds
  ordering edges from every existing reader to itself. A Canvas pass that
  writes the upload destination is a real conflict and must fail.
- An upload node is added only when `flux_frame_current()` names a recording
  frame. Uploads outside a frame are already ordered by the batch flush in
  `flux_surface_begin_frame`; do not invent a device-level current frame.

## Suggested next prompt

“Continue 1B-3A from docs/dev/ai-handoff-render-plan.md. Validate the frozen
plan at submit, enforce the budget before vkEndCommandBuffer, add tests, and do
not switch backend execution or add a second liveness table.”
