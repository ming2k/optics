# Testing

## Scope

The root Meson build owns all C tests for the libraries under `libs/`.
The Rust bindings live in the repository-root Cargo workspace and their crates
stay under `bindings/crates/` (ADR-0159, ADR-0163). Windowed examples remain
manual checks because they need a compositor and GPU surface.

The test system is a three-layer pyramid with orthogonal quality lanes
([ADR-0162](../adr/0162-layered-test-architecture-one-harness-three-layers-public-api-e2e.md)).
Every registered test declares exactly one layer or one lane. The taxonomy is
checked mechanically by `tools/check_test_layers.py`.

```text
                     ┌─────────────────────┐
                     │        e2e          │  3 binaries
                     │ public API → pixels │  slowest, smallest
                     └──────────┬──────────┘
                ┌───────────────┴────────────────┐
                │          integration           │  66 binaries
                │ Vulkan / fonts / real seams    │  environment-bound
                └───────────────┬────────────────┘
       ┌────────────────────────┴────────────────────────┐
       │                     unit                        │  112 binaries
       │ one library, CPU-only, deterministic            │  fastest, widest
       └─────────────────────────────────────────────────┘

       orthogonal lanes: bench · fuzz · lint · tooling
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
`FLUX_ALLOW_SOFTWARE_DEVICE=1`. The `device_hardware` Phase B0 gate otherwise
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
| `e2e_public_api` | Umbrella version lockstep → lens frame → snapshot → CPU pixels |
| `e2e_interaction_session` | Pointer press/release → checkbox toggle → focused text input → accessibility walk → snapshot replay |
| `e2e_document_pipeline` | SVG → reticle raster; SVG → runtime icon → lens snapshot → immutable `DisplayList` → serialize/deserialize → byte-identical pixels |

## One Harness, One Fixture Layer

Every test includes `tests/support/optics_test.h`. There is no per-suite
assertion header, no `CHECK`/`EXPECT` alias, and no second counter
implementation.

```c
#include "optics_test.h"
#include "lens_test.h"   /* only when the headless lens frame protocol is used */

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

Vulkan fixtures live in `tests/support/flux_test.h`, separately from assertions:

```c
flux_device *d = flux_test_make_headless_device();
if (!d)
    OT_SKIP("no Vulkan device");
```

The split is intentional: a CPU-only unit test links `optics_test` alone, while
`flux_test` is the one place that owns ICD pinning, validation-layer probing, and
memory-type selection.

## Topology and Registration

`tests/meson.build` is the single test orchestrator:

```text
tests/
├── meson.build          topology: support first, then each layer
├── support/             optics_test + flux_test + lens_test
├── flux/                unit/, integration/, bench/, fuzz/
├── glyph/               unit/, integration/
├── prism/               unit/, integration/
├── transit/
├── vista/
├── reticle/
├── lens/
├── iris/
├── optics/              façade, version lockstep, lint, tooling
└── e2e/                 public-API scenarios
```

Register a test in the Meson file for its layer and pass `suite : 'unit'`,
`suite : 'integration'`, or `suite : 'e2e'`. Bench and fuzz use their own suite
and are never given a layer. Test names are globally unique; a duplicate is a
guardrail failure, not a silent Meson alias.

The `lint` lane also runs tree-level gates that no single test binary can own:

| Gate | What it proves |
|------|----------------|
| `test_layers` | Every registered test has exactly one layer/lane; no private harness reappears. |
| `wayland_listeners` | Every Wayland listener event has a non-NULL slot. |
| `version_lockstep_tree` | Meson, the umbrella header, every library header, and the Cargo workspace name one version. |
| `library_purity` | Shared libraries do not write to stdout; `flux` does not reference named materials. |
| `docs_links` | Every relative documentation link resolves; public docs do not link into `docs/dev/`. |

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
FLUX_ALLOW_SOFTWARE_DEVICE=1 \
  meson test -C build --suite integration
```

On Windows and macOS without a GPU driver, run `unit`, `e2e`, `lint`, and
`tooling`; the `integration` suite is the only GPU-dependent layer.

## The Layer Guardrail

`tools/check_test_layers.py` reads `meson introspect <build> --tests` and fails
when:

- a test has no layer/lane/meta classification, or declares two layers;
- a test mixes a layer with a bench/fuzz lane;
- a test name is registered twice;
- `unit`, `integration`, or `e2e` is empty;
- the pyramid is inverted (`unit` < `e2e`);
- a private `tests/*/test_helpers.h` or `.c` reappears;
- a legacy `CHECK`/`EXPECT`/`TEST_REPORT`/`TEST_SUMMARY` assertion token
  reappears outside the shared harness;
- a source under a `unit/` directory creates a Vulkan device;
- an E2E source includes a private header or the E2E Meson file compiles a
  library source;
- an E2E source is not registered in `tests/e2e/meson.build`, or the E2E
  Meson file does not link `optics_dep`.

It runs as the `test_layers` lint/tooling test and in CI:

```bash
python3 tools/check_test_layers.py build
```

A new test that bypasses the taxonomy fails this gate even if its assertions
pass.

## Interpret Failures

| Symptom | Inspect first |
|---------|---------------|
| `flux` unit failure | The matching file in `tests/flux/unit/` and its source module. |
| Vulkan test skips | Vulkan 1.3 device discovery, ICD selection, and required extensions. |
| Vulkan validation error | The integration test's frame/resource lifetime and `libs/flux/src/`. |
| `lens` widget or layout failure | The same-named test in `tests/lens/` and `libs/lens/src/`. |
| `iris` theme or API failure | `libs/iris/src/theme_linux.c` or `libs/iris/src/app.c`. |
| E2E pixel mismatch | Start at the first failing layer in the chain: reticle raster, lens snapshot, display-list serialization, or CPU replay. |
| Layer guardrail failure | `tools/check_test_layers.py` output; the test's Meson registration and `tests/e2e/` include discipline. |
| Window is blank or examples fail to start | Wayland/GLFW setup and the relevant example; this path is not covered by headless tests. |

## Sanitizers

Use a separate build tree so sanitizer flags do not contaminate the normal
build:

```bash
meson setup build-asan -Dtests=true \
  -Db_sanitize=address,undefined
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
  `tests/support/flux_test.c`.
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

`tests/prism/golden/liquid_glass_default.pam` is a tolerance golden: a
rendered reference that `tests/prism/integration/test_prism_golden.c`
compares against with a per-channel tolerance (12/255) and a small
outlier allowance, layered over driver-independent hard invariants
(exact silhouette, edge-direction preservation, rim-brightness
direction, same-device bit-identical re-render).

Driver validity, and its limits:

- The committed reference is generated on **lavapipe** (the CI's Vulkan
  implementation). Any Mesa version can regenerate it bit-identically
  on the same implementation.
- A *different* driver (real Intel/AMD/NVIDIA hardware, MoltenVK) may
  legitimately differ by a few quantization steps — that is what the
  tolerance and the outlier allowance absorb. The hard invariants
  do the strict work everywhere else.
- If a golden comparison fails, first check `max_delta` in the test's
  stderr line: a few hundred pixels at delta ≤ 24 on new hardware is
  driver variance (tune `GOLDEN_TOLERANCE` only if the new value still
  discriminates); a systematic shift of every pixel is a real
  regression.
- Regenerate deliberately, never casually:

```bash
PRISM_GOLDEN_UPDATE=1 ./build/tests/prism/integration/test_prism_golden
git diff tests/prism/golden/   # the reference must be part of the change
```

A golden commit whose diff shows the reference but not the recipe (or
vice versa) is wrong: one visual change, one reference update, one
commit.

## Manual or Example Verification

Build examples with `-Dexamples=true`, then run the smallest relevant program:

```bash
./build/examples/flux/canvas_hello
./build/examples/lens/headless_demo
./build/examples/iris/hello_app
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
