# System Architecture & Topology

The library graph, the architectural rules, and the mechanism that enforces
each one. A rule without a mechanism is not listed here.

---

## 1. Library Graph

Nine libraries under `libs/`, plus Rust bindings under `bindings/crates/`.
Dependencies point one way. The Meson dependency declarations are the
authority; this diagram is a rendering of them.

```text
aperture ──┬──► iris ───────────────► flux
           └──► lens ──┬──► glyph ───► flux
                       └──► reticle ─► flux
vista ─────────────────────────────► flux
prism ─────────────────────────────► flux
transit   (header-only; libm only)
```

| Library | Depends on | Owns |
|---|---|---|
| `flux` | Vulkan | RHI, 2D canvas (GPU and CPU backends), 3D draw core, compute, image effects, curve kernel, distance fields, math. One library, one ABI: no optional modules. |
| `glyph` | flux | Text shaping, layout metrics, glyph atlas. |
| `reticle` | flux | SVG static-art compiler. Device-less (ADR-0136). |
| `vista` | flux | Scene graph, transforms, glTF loading. |
| `prism` | flux | Named surface materials (ADR-0063). |
| `transit` | libm | Motion vocabulary. Header-only (ADR-0158). |
| `iris` | flux | Window, event pump, frame cadence, clipboard, IME, dialogs, accessibility bridge, and the UI-host seam `<iris/host.h>`. |
| `lens` | flux, glyph, reticle | UI engine. Headless. |
| `aperture` | iris, lens | Application shell: implements `iris_ui_host` over a lens context. |

There are no numbered layers. The earlier "Layer 0–3" model implied edges that
do not exist (nothing in `prism`, `vista` or `transit` depends on `glyph`) and
hid the one that mattered (`iris` depended on `lens`). The graph above is the
whole truth (ADR-0166).

flux is large by design: it is the foundation, not a micro-kernel. It ships as
one library with every module so that its ABI is identified by its version
alone.

---

## 2. Architectural Rules

1. **One-way dependencies.** A library includes only the libraries on its row
   above. In particular `iris` never includes a UI header and `lens` never
   includes a windowing header; `aperture` is the only library that links both.
   *Mechanism:* each library's Meson target lists its dependencies, so a header
   outside them is not on the include path and the compile fails. The Windows
   translation-unit gate in `ci-cross.yml` compiles `iris` with no lens include
   path. `tests/iris` links no lens.
2. **The descriptor rule.** See [API design](api-design.md). *Mechanism:*
   `tools/check_api.py` and its shrinking baseline.
3. **No compatibility shims.** When a design is superseded, the old API is
   removed in the same change. Pre-1.0 the SONAME is the full version string,
   so nothing can load across releases by accident.

---

## 3. Red-Lines

### 3.1 Material Boundary Separation (ADR-0063)
- **Red-Line**: `flux` must NEVER reference named materials (`prism_`, `liquid_glass`, frosted glass, mica, acrylic) or higher-level material shaders.
- **Enforcement**: The Meson dependency graph gives `libs/flux` no `prism_dep` and no prism include path, so a material header or symbol fails at compile/link time. `tools/check_library_purity.py` (`meson test --suite lint`) additionally rejects named-material references in flux sources. `flux` exposes generic shader pipelines, bindless handles, and canvas render targets; high-level BRDF models belong exclusively to `prism`.

### 3.2 Vulkan RHI Purity & Headless Software Fallback (ADR-0006, ADR-0019)
- **Red-Line**: Optics maintains no runtime RHI abstraction layer. Vulkan 1.3 is the primary GPU backend.
- **Headless CPU Software Fallback**: `libs/flux` provides `<flux/canvas_cpu.h>` for GPU-free execution, headless rendering, pixel testing, and continuous integration.

### 3.3 Zero Heap Allocation in a Steady-State Frame (ADR-0024, ADR-0166)
- **Red-Line**: A frame that builds the same widgets as the previous frame runs `lens_begin` through `lens_end` without calling libc `malloc`, `calloc`, `realloc`, or `free`, whatever input arrives. Temporary storage comes from the per-frame arena, reset at frame boundaries.
- **Outside the red-line**: structural change. A node entering or leaving the tree allocates or frees its retained state, and the node store rehashes when it grows or, after a 300-frame dwell, shrinks. Snapshot publication after `lens_end` owns its captured visuals and may allocate.
- **Enforcement**: `tests/lens/test_zero_alloc.c` interposes the process allocator and counts every heap call made inside 600 steady-state frames driven with pointer motion, clicks and focus changes. The count must be zero.

### 3.4 Library Stdout Hygiene
- **Red-Line**: Shared library translation units under `libs/` must never write to `stdout` (`printf`, `puts`). Standard output is strictly reserved for application CLI or IPC wires. Diagnostic messages use `stderr`.
- **Enforcement**: `tools/check_library_purity.py` (`meson test --suite lint`) strips comments and literals, then rejects `printf`, `puts`, `putchar`, and stdout-directed `fprintf`/`fputs`/`fwrite` in shared-library translation units.

---

## 4. Review Questions for New Public API

Before adding a public function, primitive or library:

1. **Can a caller already compose it** from existing primitives? Then it belongs in the application or an example, not the library.
2. **Does it respect the graph** in §1? A new edge is an architectural change and needs an ADR.
3. **Does it pass `tools/check_api.py`** without a new baseline entry?
4. **If it runs inside the frame walk, does `test_zero_alloc` still pass?**
