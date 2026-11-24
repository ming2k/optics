# Agent Directives

## 1. Operational Guardrails (Hard Constraints)
- **Prohibited Git Commands**: NEVER execute `git stash`, `git branch`, or `git reset`.
- **Command Termination**: All commands MUST be finite and self-terminating. Never start background servers or unbounded loops without a strict timeout.
- **Clean Git State**: Do not leave scratch files, temporary binaries, or untracked test logs in the working tree.

## 2. Core Mental Model & Subsystem Boundaries
- **Topology**: Optics is a C23 graphics and UI stack of nine semantic libraries under `libs/` with unified `<optics/...>` headers and `optics_<mod>_` symbols (ADR-0170). Dependencies point one way; the build graph is the authority (`meson.build` per library):
  - `gfx` (`liboptics_gfx`, Vulkan only): Vulkan 1.3 RHI, 2D vector canvas (GPU & CPU backend `<optics/gfx/canvas_cpu.h>`), 3D draw core, compute pipelines, image effects, analytic curve kernel. One library, one ABI, no optional modules.
  - `text` (`liboptics_text`, → gfx): HarfBuzz-backed text shaping, layout metrics, and dynamic texture atlas caching.
  - `svg` (`liboptics_svg`, → gfx): Vector-art document compiler (SVG path grammar, shape decomposition, gradient resolution, headless rasterization).
  - `scene` (`liboptics_scene`, → gfx): Scene graph, cameras, hierarchical transforms, glTF loading.
  - `material` (`liboptics_material`, → gfx): Surface materials: Liquid Glass (refraction, dispersion, contact AO), Frosted Glass, Acrylic, and Mica.
  - `motion` (`<optics/motion.h>`, header-only, libm): Closed-form analytic spring integration and parametric easing.
  - `window` (`liboptics_window`, → gfx): Native OS windowing, event pump, display pacing (Wayland, Win32, Cocoa), event seam (ADR-0107), dual-thread pipeline (ADR-0124), and the UI-host seam `<optics/window/host.h>`. Must never include a UI header.
  - `ui` (`liboptics_ui`, → gfx, text, svg): Immediate-mode UI engine, layout (Grid/Flex/Flow), retained store, accessibility tree. Headless: must never include a windowing header.
  - `app` (`liboptics_app`, → window, ui): Application shell. The only library that links both `window` and `ui`; implements `optics_window_ui_host` over an `optics_ui` context.
- **Pure C23 Baseline**: Idiomatic modern C (`-std=c23`) across all modules:
  - Pointers: Always use `nullptr` (never `NULL` or `0`).
  - Zero-initialization: Always use `{}` (never `{0}`).
  - Enums: Explicit underlying types (`typedef enum name : uint32_t { ... }`).
  - Attributes: Standard C23 `[[nodiscard]]`, `[[reproducible]]`, `[[unsequenced]]`.
  - Assertions: Standard C23 `static_assert(cond, msg)`.
- **Descriptor Rule**: A public function takes at most four parameters; anything needing more configuration takes exactly one typed descriptor (`const optics_<mod>_<item>_opts *` or `_desc *`), never two. Scalar setters, getters and pure math/geometry kernels stay positional. Variant suffixes (`_ex`, `_wrapped`, `_primary`) are forbidden: a variant is a descriptor field.
- **Semantic Naming & Namespace (ADR-0170)**: Every library uses its canonical semantic name under `libs/<mod>/`, installs headers under `include/optics/`, prefixes public C symbols with `optics_<mod>_`, and publishes `optics-<mod>` crates.
- **Material Boundary Separation (ADR-0063)**: `gfx` must NEVER reference named materials (`optics_material_`, `liquid_glass`) or higher-level shaders. Enforced natively by linker and build-system dependency isolation.
- **Zero-Alloc Steady-State Frame**: A frame that builds the same widgets as the previous one runs `optics_ui_begin` ➔ `optics_ui_end` without calling libc `malloc`/`calloc`/`realloc`/`free`, whatever the input; it borrows from the per-frame arena (`ui->arena`). Structural change (a node entering or leaving, the node store resizing) may use the persistent allocator. Enforced by `tests/ui/test_zero_alloc.c`.
- **Library Stdout Purity**: Shared library TUs must never write to `stdout` (`printf`, `puts`), reserving stdout for IPC. Use `stderr` for diagnostics.

## 3. Engineering Discipline
- **Mechanisms Over Exhortations**: Rely on C23 compiler attributes (`[[nodiscard]]`, `static_assert`), link-time library separation, and automated tests to guarantee invariants. Never rely on natural-language comments alone.
- **Anti-Inflation**: Do not mint pseudo-invariant tags. Model states via typed enums, and layout metrics via design constants (`docs/dev/standards/invariants.md`).
- **Verification**: Verify changes through `meson test -C build` and `./tools/format.sh --check` before declaring tasks complete.
- **Clean Epistemic Lineage**: References must resolve to canonical in-repo ADRs (`docs/adr/0001`+).

## 4. On-Demand Standards Routing
Do not guess repository rules or rely on stale assumptions. Consult authoritative specifications as needed:
- Standards Suite Catalog: `docs/dev/standards/README.md`
  - System Architecture, Library Graph & Red-Lines: `docs/dev/standards/architecture.md`
  - Invariant & Constraint Policy: `docs/dev/standards/invariants.md`
  - C23 API Design & Opts Conventions: `docs/dev/standards/api-design.md`
  - ADR Standards & Template: `docs/dev/standards/adr.md`
  - Documentation Taxonomy & Audience Firewall: `docs/dev/standards/taxonomy.md`
  - Markdown & Hyperlink Style: `docs/dev/standards/style.md`
  - Engineering Workflow & Gates: `docs/dev/standards/workflow.md`
  - Blameless Incident Postmortems: `docs/dev/standards/postmortems.md`
- Testing Strategy & Execution: `docs/dev/testing.md`
- Manual Testing Runbooks: `docs/dev/manual-testing.md`
