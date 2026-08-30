# Optics Architecture Governance & Review Standards

This directory contains the mandatory architectural rules, review gate filters, and lifecycle governance standards for all code, modules, and public APIs within the Optics monorepo (`flux`, `lens`, `iris`, `prism`, `anim`).

---

## 1. Governance Charter & Invariants

All software in Optics adheres to four non-negotiable architectural mandates:

1. **Micro-Kernel Principle & Minimal Orthogonality**:
   - Each library owns exactly one physical domain (Rendering, UI State, Platform Shell, Motion, Material).
   - Core libraries contain **only irreducible primitives**. Compound helpers, pre-baked layouts, and aggregate widgets belong to userland component kits or applications, never the engine core.
2. **Single Opts Descriptor Paradigm**:
   - Every public functional entry point accepts **exactly one** typed descriptor struct (`const module_<item>_opts *opts`) and returns a uniform status/response object.
   - Dual API tiers (`_ex`), parameter-derived suffixes (`_wrapped`, `_primary`, `_vertical`), and floating state modifiers are strictly forbidden.
3. **Strict Layered Separation**:
   - Spatial overlay escape (`Place`) $\neq$ Viewport translation (`Scroll`) $\neq$ Geometric layout flow (`Row`/`Column`/`Grid`) $\neq$ Data interaction atoms (`Button`/`Textedit`/etc.).
4. **Zero-Baggage Deprecation Policy**:
   - When a design is superseded by an orthogonal primitive, the obsolete API is completely excised. No shims, deprecated aliases, or compatibility redirects are retained in the core library.

---

## 2. Multi-Domain Governance Specifications

Specific intake criteria and technical invariants are governed per domain:

| Domain | Governance Specification | Governed Modules | Core Concern |
| :--- | :--- | :--- | :--- |
| **UI Components** | [`components.md`](components.md) | `libs/lens`, `bindings/lens-rs` | Interaction data types, state machines, A11y, 4-layer purity |
| **API Design** | [`api-design-guidelines.md`](api-design-guidelines.md) | All Monorepo APIs | Single Opts descriptor rules, C99 compound literals, naming |
| **Image Effects** | [`effects.md`](effects.md) | `libs/flux/effect`, `prism` | GPU pixel operators, bandwidth bounds, zero choreography |
| **Surface Materials** | [`materials.md`](materials.md) | `libs/prism`, `flux` | Physical BRDF models, shader register bounds, offscreen graphs |
| **Platform Backends** | [`backends.md`](backends.md) | `libs/iris` | Native OS windowing, non-invasive event pump, zero core glue |
| **Documentation** | [`documentation/index.md`](documentation/index.md) | `docs/` | 4-Gate routing cascade, Diátaxis, style guide, ADRs |

---

## 3. The 5-Gate Review Matrix

Every PR proposing a new API, primitive, or component must pass through the 5-Gate Filter:

```
[ Proposal ] ──▶ [ Gate 1: Irreducibility ] ──▶ [ Gate 2: Data Orthogonality ]
                        │                              │
                     Pass?                          Pass?
                        ▼                              ▼
                 [ Gate 3: Layer Purity ]   ──▶ [ Gate 4: API Compliance ]
                        │                              │
                     Pass?                          Pass?
                        ▼                              ▼
                 [ Gate 5: Zero-Alloc Execution ] ──▶ [ Merge into Core ]
```

1. **Gate 1: Irreducibility**: Can this be built by nesting existing primitives in userland? (If YES ➔ Reject from core).
2. **Gate 2: Data Orthogonality**: Does this introduce an unrepresented fundamental interaction type (0D pulse, 0D boolean, 1D scalar, 1D/2D string)? (If NO ➔ Merge into existing primitive's `.variant` or `.appearance`).
3. **Gate 3: Layer Purity**: Does it belong exclusively to Overlay, Viewport, Layout, or Atom? (If NO ➔ Reject cross-layer hybrid).
4. **Gate 4: API Compliance**: Does it adhere to the Single Opts Descriptor paradigm? (If NO ➔ Reject `_ex` or fragmented suffixes).
5. **Gate 5: Zero-Alloc Execution**: Does the render walk run with zero heap allocation (`malloc`), using only the frame arena? (If NO ➔ Reject).

---

## 4. Contributor PR Verification Checklist

- [ ] Passed the 5-Gate Review Filter.
- [ ] Conforms to [`api-design-guidelines.md`](api-design-guidelines.md).
- [ ] Accompanied by an ADR in `docs/adr/` (if architectural).
- [ ] 100% test pass rate with ASan / UBSan enabled (`meson test -C build`).
- [ ] Updated Rust safe wrapper in `bindings/` (tested with `cargo test`).
- [ ] Regenerated symbol index (`python3 tools/gen_symbols.py`).
