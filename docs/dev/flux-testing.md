# Testing

## Running the suite

    meson test -C build

Output includes pass/fail and per-test timing. Full log in
`build/meson-logs/testlog.txt`.

| Subcommand                                    | Effect                                |
|-----------------------------------------------|---------------------------------------|
| `meson test -C build`                         | All tests.                            |
| `meson test -C build --suite unit`            | Only unit tests.                      |
| `meson test -C build math`                    | Only the named test.                  |
| `meson test -C build --print-errorlogs`       | Show stderr on failure (default).     |
| `meson test -C build --wrap='valgrind --leak-check=full'` | Run under valgrind.       |

## Adding a unit test

| Step                                                                    |
|-------------------------------------------------------------------------|
| Create `tests/test_<name>.c`.                                           |
| Use `EXPECT(cond)` / `EXPECT_NEAR(a, b, eps)` from `test_helpers.h`.    |
| End with `TEST_SUMMARY()`.                                              |
| Add the test executable and `test()` to `tests/meson.build`.            |
| Run `meson test -C build` to confirm it passes.                         |

Example:

    #include <flux/flux.h>
    #include "test_helpers.h"

    int main(void) {
        EXPECT(1 + 1 == 2);
        EXPECT_NEAR(3.14, 3.14159, 0.01);
        TEST_SUMMARY();
    }

## What to test

| You added                                  | Add a test that...                                  |
|--------------------------------------------|-----------------------------------------------------|
| A public function                          | exercises the documented contract with a value, an edge case, and a NULL input. |
| A math helper                              | verifies a known input/output pair from first principles. |
| A new struct or enum                       | checks default values via the documented constructor. |
| A behavioural change in an existing function | adds a regression test before fixing.             |

## What not to test

- Vulkan calls themselves — the validation layer is the test.
- `#embed` shader binaries — sources are compiled deterministically on every build.
- Window system integration — that is the example programs' job.

## Sanitizer builds

    meson setup build-asan -Db_sanitize=address,undefined -Dtests=true
    meson test -C build-asan

CI should run both a normal build and an ASan/UBSan build.

## Headless CI

Lavapipe provides software Vulkan, enough to run the GPU-touching
tests without a display. The CI job in `.github/workflows/ci.yml`
shows the apt packages needed (`mesa-vulkan-drivers`,
`vulkan-validationlayers`) and the `VK_ICD_FILENAMES` override that
forces the lavapipe ICD.
