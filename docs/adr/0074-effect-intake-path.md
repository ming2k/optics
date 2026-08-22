# ADR-0074: Effect intake path — new visual operators and where choreography never enters

- Status: Proposed
- Date: 2026-08-22
- Scope: flux `effect` module, prism; consumes the placement rule the Aegis
  compositor records as its ADR-0139

## Context

Optics renders for consumers whose product surface needs effects: the Aegis
desktop compositor composites chrome over live client content using the
blur operator, the liquid-glass material, and caller-owned spring
choreography. Effect requests keep arriving with the same shape — "add a
drop shadow", "add a glow", "add particles" — and each one re-opens the
same two questions: which library owns it, and what does the consumer owe
back?

The boundary is already written inside Optics (ADR-0047: "flux owns
mechanism, the caller owns policy, the shader owns the material identity";
ADR-0061: mechanism / neutral default / flavor), but there is no intake
path — no statement of what a new operator arrives *with*, so every
request is negotiated from scratch and the cross-repo cost (bindings,
docs, symbol table, tag, consumer lockfile) lands as a surprise late in
the process.

On the consumer side, Aegis has now recorded the mirror decision
(Aegis ADR-0139): pixel-touching work belongs here; choreography
(springs, staggering, reveals) belongs there, on host-owned clocks. The
two rules interlock: this ADR fixes what Optics will accept as an effect
operator and what it will permanently refuse.

## Decision

1. **New image-domain operators (shadow, glow, tone-map, new blur modes)
   land in the flux `effect` module** (`libs/flux/src/effect/`), following
   ADR-0008's charter and ADR-0047's boundary: no product concept may
   appear in the operator, every caller-definable knob goes in the
   descriptor (dispatch-wide) or group (per-body), and identity curves
   stay shader constants.

2. **New material looks land in prism** (`libs/prism/`), as named
   materials on the public effect runtime, not as new modes inside an
   existing shader.

3. **Choreography never enters Optics.** No tween, easing, timeline, or
   animation library is added to any Optics layer; clocks stay
   host-owned. Animated parameters are expressed the way blur sigma
   already is: values the caller re-supplies each frame, with a
   fixed-cost path (the Dual-Kawase pyramid's constant loop length) so
   per-frame variation stays cheap. Flavor ships as copyable recipes
   under `examples/showcase/` (ADR-0061), never behind a library flag.

4. **The intake checklist for a new operator:** a C header surface in
   the owning library's public headers with descriptor-ABI extension
   chains (`sType`/`next`, Aegis-relevant per `docs/reference/api.md`);
   the Rust binding mirrored 1:1 in `bindings/<lib>-rs`; an entry in
   `docs/reference/` and the symbol table; a test under `tests/<lib>/`;
   and a shader-build integration (rgba8 and `EFFECT_STORAGE_RGBA16F`
   variants where the operator samples color, per ADR-0069). An operator
   is not "done" until all five exist.

5. **Animation-safety is part of the operator contract.** Any operator a
   caller might drive per-frame (shadow offset/blur during a window
   drag, glow strength during a hover) must document its fixed vs
   varying cost, prefer dispatch-shape-stable execution (no pipeline or
   allocation churn when only float parameters change), and lease
   transient images through the existing per-device pool with
   `flux_effect_reset` epochs rather than caller-managed intermediates.

## Alternatives Considered

- **A tween/timeline library in lens or flux.** Rejected: it
  contradicts "flux deliberately does not own an animation timeline" and
  lens's "mechanism, not animation" charter; every Optics consumer would
  pay for one consumer's choreography, and Aegis's ADR-0139 explicitly
  wants host-owned clocks.
- **Accepting effects as consumer-side recipes only.** Rejected: an
  operator needing bindless images, compute pipelines, and frame-slot
  barriers re-plumbed above flux is worse coupling than a parameter set
  (ADR-0047 already rejected the split-out).
- **A generic effect-graph/chaining object.** Rejected in ADR-0008;
  chaining stays caller-side. This ADR does not reopen it: a graph object
  is a second policy surface, which is exactly what ADR-0047 keeps out.
- **Presets per product surface (dock preset, chip preset).** Rejected
  in ADR-0047; presets are how product knowledge leaks into the library.

## Consequences

- The next proposed operator (a `flux_effect_shadow` remains the
  top-candidate gap, named aspirationally in `flux/effect.h`'s tagline
  since ADR-0008) enters through the five-item checklist above instead
  of an ad hoc review. Push-constant budget and descriptor ABI growth
  must be checked at intake (ADR-0065's per-group packing precedent).
- Consumers get a stable rule for what they may ask for: pixel-touching
  capability arrives as an operator here; taste (springs, stagger,
  reveals) stays in their chrome on their clock.
- Bindings, reference docs, and tests grow with every operator — the
  checklist makes that cost visible at proposal time rather than after
  review.
- This ADR is recorded as Proposed until the first operator lands under
  it; acceptance rides with that implementation.

## References

- ADR-0008 (image-effect pipeline), ADR-0046/0063/0065 (glass model,
  material library, per-group overrides), ADR-0069 (color-managed effect
  storage), ADR-0061 (mechanism / neutral default / flavor)
- Aegis ADR-0139 (consumer-side mirror: Optics mechanism, Aegis policy)
- `examples/flux/image_animation.c` — the host-owned-clock stance in
  practice
