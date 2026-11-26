# Testing

## Scope

The root Meson build owns all C tests for the libraries under `libs/`.
The Rust crates live at repository-root `crates/` under the root Cargo
workspace (ADR-0159, ADR-0163, ADR-0169). Windowed examples remain
manual checks because they need a compositor and GPU surface.

The test system is a three-layer pyramid with orthogonal quality lanes
([ADR-0162](../adr/0162-layered-test-architecture-one-harness-three-layers-public-api-e2e.md)).
Every registered test declares one layer (`unit`, `integration`, `e2e`) or one
orthogonal lane (`bench`, `fuzz`, `lint`).

```text
                     ┌─────────────────────┐
                     │        e2e          │  public API → pixels
                     │  headless CPU-only  │  smallest, broadest
                     └──────────┬──────────┘
                ┌───────────────┴────────────────┐
                │          integration           │  Vulkan / fonts / GPU stack
                │  real boundaries & readback    │  environment-bound
                └───────────────┬────────────────┘
       ┌────────────────────────┴────────────────────────┐
       │                     unit                        │  one library, CPU-only
       │   deterministic, fast, zero external deps       │  widest base
       └─────────────────────────────────────────────────┘

       orthogonal lanes: bench · fuzz · lint
```

## Layer Contract

| Layer | Location | May use | Must not use | Command |
|-------|----------|---------|--------------|---------|
| `unit` | `tests/<library>/unit/` and CPU-only files in `tests/<library>/` | One library, hidden implementation TU when a symbol is intentionally private, CPU canvas | Vulkan device, compositor, network, subprocess | `meson test -C build --suite unit` |
| `integration` | `tests/<library>/integration/` and cross-boundary files | Vulkan 1.3 device or Lavapipe, font stack, multi-library seam, filesystem/process boundary | Private production APIs that are not part of the seam under test | `meson test -C build --suite integration` |
| `e2e` | `tests/e2e/` | Public headers, `optics_dep`, CPU canvas | Any `libs/*/src` include, `internal.h`, compiled-in library source, compositor, GPU | `meson test -C build --suite e2e` |

`bench`, `fuzz`, `lint`, and `tooling` are not layers. A benchmark is not a unit
test with a timer; a fuzz harness is not an integration test with random input.
They are registered under their own suites and run in dedicated jobs.

### Unit

A unit test proves one library's contract without an external boundary. It is
deterministic, CPU-only, and must stay green on a host with no Vulkan ICD and no
display server. A test may compile a hidden implementation translation unit into
its own binary when the alternative is exporting an internal symbol solely for
testing; that is still a unit test because no external system is involved.

### Integration

An integration test crosses a real boundary: a Vulkan device, the font stack,
two libraries meeting at a public seam, or a filesystem/process boundary. It may
skip when the environment cannot supply that boundary. A skip is Meson exit code
77, reported by `OT_SKIP` / `OT_SKIP_IF`, never a silent `return 0`.

`canvas_consistency` remains the portability gate for the GPU canvas: it
compares the GPU backend against the CPU oracle pixel-by-pixel and is expected
to pass on every platform's Vulkan driver (MoltenVK >= 1.3 on macOS). See
[Cross-platform](cross-platform.md).

When the integration suite runs on a software Vulkan ICD (Lavapipe), set
`OPTICS_GFX_ALLOW_SOFTWARE_DEVICE=1`. The `device_hardware` Phase B0 gate otherwise
fails by design on llvmpipe; with the variable set it still reports the detected
device and asserts the compute prerequisites.

### End-to-end

An E2E test uses the public API the way an application does and asserts an
observable result: pixels, an accessibility tree, a serialized byte stream, or a
process exit contract. It may include public sub-headers, but never
`libs/*/src`, `internal.h`, or a compiled-in library source. E2E is deliberately
the smallest layer: it catches composition failures that unit and integration
tests cannot see, and it is too coarse to diagnose a widget or a shader on its
own.

Current E2E scenarios:

| Test | Chain under assertion |
|------|-----------------------|
| `e2e_gfx_version` | Umbrella version → `gfx` runtime version, with no UI/canvas requirement |
| `e2e_public_api` | Umbrella version lockstep → UI frame → snapshot → CPU pixels |
| `e2e_interaction_session` | Pointer press/release → checkbox toggle → focused text input → accessibility walk → per-widget pixel diff |
| `e2e_motion_session` | Analytic spring + hysteresis → multi-frame UI snapshots → monotonic visual motion & reduced-motion byte-identical snap |
| `e2e_gpu_stack` (`integration`) | Public-API-only full GPU stack: backdrop → Dual-Kawase blur → animated spring + HUD UI + registered vector icon → Vulkan readback |

## One Harness, One Fixture Layer

Every test includes `tests/support/optics_test.h`. There is no per-suite
assertion header, no `CHECK`/`EXPECT` alias, and no second counter
implementation.

```c
#include "optics_test.h"
#include "ui_test.h"   /* only when the headless ui frame protocol is used */

int main(void) {
    OT_EXPECT(1 + 1 == 2);
    OT_EXPECT_NEAR(0.1f + 0.2f, 0.3f, 1e-6);
    OT_EXPECT_STR_EQ("a", "a");
    return OT_REPORT();
}
```

| Macro | Meaning |
|-------|---------|
| `OT_EXPECT(cond)` | Count one check; on failure print `file:line` and continue |
| `OT_EXPECT_NEAR(a, b, eps)` | Numeric tolerance; NaN always fails |
| `OT_EXPECT_STR_EQ(a, b)` | Null-safe string equality |
| `OT_EXPECT_MEM_EQ(a, b, n)` | Byte equality with the first differing offset |
| `OT_REQUIRE(cond)` | Fatal precondition; report and end the test process |
| `OT_SKIP(reason)` | Meson skip (exit 77) |
| `OT_SKIP_IF(cond, reason)` | Conditional skip |
| `OT_REPORT()` | Print the summary and return the process exit code |

Diagnostics go to `stderr`. The harness never writes `stdout`; `stdout` belongs
to the program under test and to benchmarks.

Vulkan fixtures live in `tests/support/gfx_test.h`, separately from assertions:

```c
optics_gfx_device *d = gfx_test_make_headless_device();
if (!d)
    OT_SKIP("no Vulkan device");
```

The split is intentional: a CPU-only unit test links `optics_test` alone, while
`gfx_test` is the one place that owns ICD pinning, validation-layer probing, and
memory-type selection.

## Topology and Registration

`tests/meson.build` is the single test orchestrator:

```text
tests/
├── meson.build          topology: support first, then each layer
├── support/             optics_test + gfx_test + ui_test
├── gfx/                 unit/, integration/, bench/, fuzz/
├── text/                unit/, integration/
├── material/            unit/, integration/
├── motion/
├── scene/
├── svg/
├── ui/
├── window/
├── app/
├── optics/              façade, version lockstep, lint, tooling
└── e2e/                 public-API scenarios
```

Register a test in the Meson file for its layer and pass `suite : 'unit'`,
`suite : 'integration'`, or `suite : 'e2e'`. Bench and fuzz use their own suite
and are never given a layer. Test names are globally unique; a duplicate is a
guardrail failure, not a silent Meson alias.

The `lint` lane runs essential tree-level gates that no single test binary can own:

| Gate | What it proves |
|------|----------------|
| `wayland_listeners` | Every Wayland listener event has a non-NULL slot (prevents `libwayland` aborts). |
| `version_lockstep_tree` | Meson, the umbrella header, every library header, and the Cargo workspace name one version. |
| `library_purity` | Shared libraries do not write to stdout; `gfx` does not reference named materials. |
| `symbol_reference` | `docs/reference/symbols.md` matches the exported public headers. |

## Running Tests

Enable tests when configuring:

```bash
meson setup build -Dtests=true -Dexamples=true
meson compile -C build
```

Run the layers explicitly:

```bash
meson test -C build --suite unit           # CPU-only, no ICD required
meson test -C build --suite integration    # Vulkan/font/environment seams
meson test -C build --suite e2e            # public API -> observable output
meson test -C build --suite lint           # tree-level checks
meson test -C build --suite bench -v       # benchmarks only
```

Common selections:

```bash
meson test -C build --no-suite bench --no-suite fuzz  # correctness gate
meson test -C build test_theme                         # one named test
meson test -C build -v test_theme                      # verbose single test
```

`meson test -C build` also runs benchmarks unless excluded. Use
`--no-suite bench --no-suite fuzz` for the normal correctness gate. Full logs are
written to `build/meson-logs/testlog.txt`.

On a software Vulkan ICD:

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
OPTICS_GFX_ALLOW_SOFTWARE_DEVICE=1 \
  meson test -C build --suite integration
```

On Windows and macOS without a GPU driver, run `unit`, `e2e`, and `lint`; the
`integration` suite is the only GPU-dependent layer.

## Interpret Failures

| Symptom | Inspect first |
|---------|---------------|
| `gfx` unit failure | The matching file in `tests/gfx/unit/` and its source module. |
| Vulkan test skips | Vulkan 1.3 device discovery, ICD selection, and required extensions. |
| Vulkan validation error | The integration test's frame/resource lifetime and `libs/gfx/src/`. |
| `ui` widget or layout failure | The same-named test in `tests/ui/` and `libs/ui/src/`. |
| `window` theme or API failure | `libs/window/src/theme_linux.c` or `libs/window/src/app.c`. |
| E2E pixel mismatch | Start at the first failing layer in the chain: svg raster, ui snapshot, display-list serialization, or replay. |
| Window is blank or examples fail to start | Wayland/Vulkan setup and the relevant example; this path is not covered by headless tests. |

## Sanitizers

Use a separate build tree so sanitizer flags do not contaminate the normal
build:

```bash
meson setup build-asan -Dtests=true \
  -Db_sanitize=address,undefined -Db_lundef=false
meson compile -C build-asan
meson test -C build-asan --no-suite bench --no-suite fuzz
```

Standalone leak inspection (`-Db_sanitize=leak`) and thread safety
(`-Db_sanitize=thread`) are also supported.

### Unified Sanitizer Test Setup

Whenever any sanitizer is configured, Meson automatically activates the default
`sanitizer` test setup across all test suites:

- **Suppression Binding (`LSAN_OPTIONS`)**: Automatically resolves and prepends
  `tools/config/lsan.supp`, suppressing upstream process-lifetime pattern caches
  (`fontconfig`). Vulkan ICD caches are pinned with `RTLD_NODELETE` in
  `tests/support/gfx_test.c`.
- **Diagnostic Hardening**: Prepends `ASAN_OPTIONS=abort_on_error=1:halt_on_error=0`
  and `UBSAN_OPTIONS=print_stacktrace=1` to ensure failures produce immediate
  backtraces.
- **Timeout Scaling**: Applies `timeout_multiplier: 4` to prevent false positive
  test timeouts caused by shadow memory and instrumentation overhead.

Running `meson test` inherits this environment natively without manual `export`
or CLI flags. External library noise is pre-filtered; always triage the first
stack frame in project code before modifying suppression rules.

## Rust Workspaces

Configure and build the C stack first, then point each binding workspace at the
same source and build roots:

```bash
cargo test --workspace
cargo clippy --workspace --all-targets -- -D warnings
```

The build scripts use `build/meson-uninstalled/*.pc`, so no local install is
required. Rust tests are a separate Cargo layer; they are not registered in the
C Meson taxonomy.

## Add a Test

Use this decision tree:

1. **Does the test prove one library's behaviour without a device, compositor, or
   subprocess?** Put it in the library's `unit` file/suite and link
   `optics_test_dep`.
2. **Does it need a Vulkan device, the font stack, or two libraries meeting at a
   real boundary?** Put it in `integration`, link the boundary's public
   dependencies, and call `OT_SKIP` when the boundary is unavailable.
3. **Does it need to prove the public umbrella path from input to an observable
   result?** Add one scenario to `tests/e2e/`, link `optics_dep`, and keep it
   public-header-only.
4. **Is it a benchmark or fuzz harness?** Register it under `bench` or `fuzz`;
   do not give it a pyramid layer.
5. **Is it a tree-level contract scan?** Register it under `lint`/`tooling`.

Then add a stable, globally unique name in the owning `meson.build`, run the
layer guardrail, and run the exact layer you touched.

## Golden References

`tests/gfx/integration/test_compute_golden.c` renders a known pattern through
the compute pipeline and asserts the read-back pixels exactly. Golden checks
must be paired with driver-independent invariants so that a legitimate
driver difference is distinguishable from a regression. A change to a golden
expectation belongs in the same commit as the visual change that motivated it.

## Manual or Example Verification

Build examples with `-Dexamples=true`, then run the smallest relevant program:

```bash
./build/examples/getting_started/canvas_window
./build/examples/getting_started/headless_canvas /tmp/out.ppm
./build/examples/getting_started/hello_app
```

Record the compositor, driver, and GPU when reporting a visual or
platform-specific result. For the complete interactive inspection protocol,
material test cases, and cross-platform matrix, see [Manual testing](manual-testing.md).

## See Also

- [ADR-0162: Layered test architecture](../adr/0162-layered-test-architecture-one-harness-three-layers-public-api-e2e.md)
- [Manual testing](manual-testing.md)
- [Setup](setup.md)
- [Project layout](project-layout.md)
- [Application architecture](../explanation/application-architecture.md)
