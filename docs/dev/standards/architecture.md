# System Architecture & Topology

The library graph, the architectural rules, and the mechanism that enforces
each one. A rule without a mechanism is not listed here.

---

## 1. Library Graph

Nine libraries under `libs/`, plus Rust crates under `crates/`.
Dependencies point one way. The Meson dependency declarations are the
authority; this diagram is a rendering of them.

```text
app ──┬──► window ───────────────► gfx
      └──► ui ──┬──► text ───────► gfx
                └──► svg ────────► gfx
scene ───────────────────────────► gfx
material ────────────────────────► gfx
motion   (header-only; libm only)
```

| Library | Depends on | Owns |
|---|---|---|
| `gfx` (`liboptics_gfx`) | Vulkan | RHI, 2D canvas (GPU and CPU backends), 3D draw core, compute, image effects, curve kernel, distance fields, math. One library, one ABI: no optional modules. |
| `text` (`liboptics_text`) | gfx | Text shaping, layout metrics, glyph atlas. |
| `svg` (`liboptics_svg`) | gfx | SVG static-art compiler. Device-less (ADR-0136). |
| `scene` (`liboptics_scene`) | gfx | Scene graph, transforms, glTF loading. |
| `material` (`liboptics_material`) | gfx | Named surface materials (ADR-0063). |
| `motion` (`<optics/motion.h>`) | libm | Motion vocabulary. Header-only (ADR-0158). |
| `window` (`liboptics_window`) | gfx | Window, event pump, frame cadence, clipboard, IME, dialogs, accessibility bridge, and the UI-host seam `<optics/window/host.h>`. |
| `ui` (`liboptics_ui`) | gfx, text, svg | UI engine. Headless. |
| `app` (`liboptics_app`) | window, ui | Application shell: implements `optics_window_ui_host` over an `optics_ui` context. |

There are no numbered layers. The earlier "Layer 0–3" model implied edges that
do not exist (nothing in `material`, `scene` or `motion` depends on `text`) and
hid the one that mattered (`window` depended on `ui`). The graph above is the
whole truth (ADR-0166, ADR-0170).

`gfx` is large by design: it is the foundation, not a micro-kernel. It ships as
one library with every module so that its ABI is identified by its version
alone.

---

## 2. Architectural Rules

1. **One-way dependencies.** A library includes only the libraries on its row
   above. In particular `window` never includes a UI header and `ui` never
   includes a windowing header; `app` is the only library that links both.
   *Mechanism:* each library's Meson target lists its dependencies, so a header
   outside them is not on the include path and the compile fails. The Windows
   translation-unit gate in `ci-cross.yml` compiles `window` with no `ui` include
   path. `tests/window` links no `ui`.
2. **The descriptor rule.** See [API design](api-design.md). *Mechanism:*
   `tools/check_api.py` and its shrinking baseline.
3. **Semantic naming and unified `optics` namespace (ADR-0170).** Every library
   uses its canonical semantic name under `libs/<mod>/`, installs headers under
   `include/optics/`, prefixes public C symbols with `optics_<mod>_`, and
   publishes `optics-<mod>` crates. *Mechanism:* `tools/check_naming.py`.
4. **No compatibility shims.** When a design is superseded, the old API is
   removed in the same change. Pre-1.0 the SONAME is the full version string,
   so nothing can load across releases by accident.

---

## 3. Red-Lines

### 3.1 Material Boundary Separation (ADR-0063)
- **Red-Line**: `gfx` must NEVER reference named materials (`optics_material_`, `liquid_glass`, frosted glass, mica, acrylic) or higher-level material shaders.
- **Enforcement**: The Meson dependency graph gives `libs/gfx` no `material_dep` and no material include path, so a material header or symbol fails at compile/link time. `tools/check_library_purity.py` (`meson test --suite lint`) additionally rejects named-material references in `gfx` sources. `gfx` exposes generic shader pipelines, bindless handles, and canvas render targets; high-level BRDF models belong exclusively to `material`.

### 3.2 Vulkan RHI Purity & Headless Software Fallback (ADR-0006, ADR-0019)
- **Red-Line**: Optics maintains no runtime RHI abstraction layer. Vulkan 1.3 is the primary GPU backend.
- **Headless CPU Software Fallback**: `libs/gfx` provides `<optics/gfx/canvas_cpu.h>` for GPU-free execution, headless rendering, pixel testing, and continuous integration.

### 3.3 Zero Heap Allocation in a Steady-State Frame (ADR-0024, ADR-0166)
- **Red-Line**: A frame that builds the same widgets as the previous frame runs `optics_ui_begin` through `optics_ui_end` without calling libc `malloc`, `calloc`, `realloc`, or `free`, whatever input arrives. Temporary storage comes from the per-frame arena, reset at frame boundaries.
- **Outside the red-line**: structural change. A node entering or leaving the tree allocates or frees its retained state, and the node store rehashes when it grows or, after a 300-frame dwell, shrinks. Snapshot publication after `optics_ui_end` owns its captured visuals and may allocate.
- **Enforcement**: `tests/ui/test_zero_alloc.c` interposes the process allocator and counts every heap call made inside 600 steady-state frames driven with pointer motion, clicks and focus changes. The count must be zero.

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
