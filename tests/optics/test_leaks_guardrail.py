#!/usr/bin/env python3
"""Negative tests for the leak-coverage guardrail (ADR-0130).

A guardrail nobody has seen fail is indistinguishable from a guardrail that
cannot fail. Each case below constructs the exact condition the guardrail
exists to catch, in a throwaway tree, and requires it to report.

Run: python3 tests/optics/test_leaks_guardrail.py
"""

import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT))

from tools.guardrails import leaks  # noqa: E402

HEADER_TEMPLATE = """
#ifndef FIXTURE_H
#define FIXTURE_H
#include <stdint.h>
#define FIXTURE_API
{body}
#endif
"""


class Fixture:
    """A minimal repo tree: one library header, the two ledgers, one test."""

    def __init__(self, tmp: Path):
        self.root = tmp
        (tmp / "libs" / "demo" / "include").mkdir(parents=True)
        (tmp / "tools" / "config").mkdir(parents=True)
        (tmp / "tests" / "demo").mkdir(parents=True)
        (tmp / "tests" / "demo" / "test_lane.c").write_text("int main(void){return 0;}\n")
        (tmp / "tests" / "demo" / "meson.build").write_text(
            "test_lane = executable('test_lane', 'test_lane.c')\n"
            "test('lane', test_lane)\n"
        )

    def header(self, body: str):
        (self.root / "libs" / "demo" / "include" / "demo.h").write_text(
            HEADER_TEMPLATE.format(body=textwrap.dedent(body))
        )

    def lanes(self, body: str):
        (self.root / "tools" / "config" / "leak-lanes.toml").write_text(
            textwrap.dedent(body)
        )

    def exempt(self, body: str):
        (self.root / "tools" / "config" / "leak-exempt.toml").write_text(
            textwrap.dedent(body)
        )


class LeakGuardrailTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.tmp = Path(self._tmp.name)
        self.fx = Fixture(self.tmp)

    def tearDown(self):
        self._tmp.cleanup()

    def errors(self):
        return leaks.check_leaks(self.tmp)

    def assert_error_contains(self, needle):
        errs = self.errors()
        self.assertTrue(
            any(needle in e for e in errs),
            f"expected an error containing {needle!r}, got: {errs}",
        )

    # -- coverage ---------------------------------------------------------

    def test_uncovered_kind_fails(self):
        self.fx.header("""
            FIXTURE_API void demo_release(void *p);
            FIXTURE_API int demo_create(void **out);
        """)
        self.fx.lanes("")
        self.fx.exempt("")
        self.assert_error_contains("uncovered resource kind: demo")

    def test_covered_kind_passes(self):
        self.fx.header("""
            FIXTURE_API void demo_release(void *p);
            FIXTURE_API int demo_create(void **out);
        """)
        self.fx.lanes("""
            [kind.demo]
            scope    = "device"
            lifetime = "context"
            lane     = "tests/demo/test_lane.c"
        """)
        self.fx.exempt("")
        self.assertEqual(self.errors(), [])

    def test_reasoned_exemption_passes(self):
        self.fx.header("""
            FIXTURE_API int arena_path_create(void **out, void *arena);
        """)
        self.fx.lanes("")
        self.fx.exempt("""
            [exempt.arena_path]
            reason = "arena-owned; the arena is the release verb"
            substitute = "arena balance assertion"
        """)
        # `arena_path` is not matched by the kind regex (needs _create suffix
        # on a base), so declare it under its own name and check the ledger
        # accepts it rather than reporting it stale.
        self.assertEqual(
            [e for e in self.errors() if "stale ledger entry" in e], []
        )

    def test_exemption_without_reason_fails(self):
        self.fx.header("""
            FIXTURE_API int demo_create(void **out);
        """)
        self.fx.lanes("")
        self.fx.exempt("""
            [exempt.demo]
            substitute = "something"
        """)
        self.assert_error_contains("exemption without a `reason` field")

    def test_stale_lane_entry_fails(self):
        self.fx.header("""
            FIXTURE_API void other_release(void *p);
            FIXTURE_API int other_create(void **out);
        """)
        self.fx.lanes("""
            [kind.demo]
            scope    = "device"
            lifetime = "context"
            lane     = "tests/demo/test_lane.c"

            [kind.other]
            scope    = "device"
            lifetime = "context"
            lane     = "tests/demo/test_lane.c"
        """)
        self.fx.exempt("")
        self.assert_error_contains("stale ledger entry: demo")

    def test_missing_lane_file_fails(self):
        self.fx.header("""
            FIXTURE_API void demo_release(void *p);
            FIXTURE_API int demo_create(void **out);
        """)
        self.fx.lanes("""
            [kind.demo]
            scope    = "device"
            lifetime = "context"
            lane     = "tests/demo/test_nonexistent.c"
        """)
        self.fx.exempt("")
        self.assert_error_contains("lane file does not exist")

    def test_unregistered_lane_fails(self):
        """A lane that is never executed is not coverage."""
        self.fx.header("""
            FIXTURE_API void demo_release(void *p);
            FIXTURE_API int demo_create(void **out);
        """)
        (self.tmp / "tests" / "demo" / "test_orphan.c").write_text("int main(void){return 0;}\n")
        self.fx.lanes("""
            [kind.demo]
            scope    = "device"
            lifetime = "context"
            lane     = "tests/demo/test_orphan.c"
        """)
        self.fx.exempt("")
        self.assert_error_contains("is not registered as a meson test")

    def test_laned_kind_without_release_verb_fails(self):
        """A kind nothing can release has no lane to point at."""
        self.fx.header("""
            FIXTURE_API int demo_create(void **out);
        """)
        self.fx.lanes("""
            [kind.demo]
            scope    = "device"
            lifetime = "context"
            lane     = "tests/demo/test_lane.c"
        """)
        self.fx.exempt("")
        self.assert_error_contains("has no release verb but is registered with a lane")

    # -- multi-line declarations -----------------------------------------

    def test_wrapped_declaration_is_detected(self):
        """Declarations reformatted across lines must still be inventoried.

        A per-line scan silently drops these — the same staleness the
        guardrail exists to prevent.
        """
        self.fx.header("""
            FIXTURE_API void
            demo_release(void *p);
            FIXTURE_API int
            demo_create(void **out);
        """)
        self.fx.lanes("")
        self.fx.exempt("")
        self.assert_error_contains("uncovered resource kind: demo")

    # -- attribution machinery -------------------------------------------

    def test_attribution_machinery_in_plain_build_fails(self):
        """A default build must not carry the name-format machinery."""
        build = self.tmp / "build-fix"
        (build / "meson-info").mkdir(parents=True)
        (build / "libs" / "flux").mkdir(parents=True)
        (build / "meson-info" / "intro-buildoptions.json").write_text(
            '[{"name":"observe_attribution","value":false}]'
        )
        lib = build / "libs" / "flux" / "libflux.so.0.0.73"
        lib.write_text("")

        fake_bin = self.tmp / "bin"
        fake_bin.mkdir()
        fake_strings = fake_bin / "strings"
        # Report the attribution format template as if it were compiled in.
        fake_strings.write_text(
            '#!/bin/sh\n'
            'echo "%s.%s.%s"\n'
            'echo "flux_image_create"\n'
        )
        fake_strings.chmod(0o755)

        self.fx.header("""
            FIXTURE_API void demo_release(void *p);
            FIXTURE_API int demo_create(void **out);
        """)
        self.fx.lanes("""
            [kind.demo]
            scope    = "device"
            lifetime = "context"
            lane     = "tests/demo/test_lane.c"
        """)
        self.fx.exempt("")

        import os
        old_path = os.environ.get("PATH", "")
        os.environ["PATH"] = f"{fake_bin}:{old_path}"
        try:
            self.assert_error_contains("attribution name machinery present")
        finally:
            os.environ["PATH"] = old_path

    # -- forbidden spellings ---------------------------------------------

    def test_raw_close_in_iris_fails(self):
        """A detector with an unguarded escape hatch is escaped from."""
        (self.tmp / "libs" / "flux" / "src").mkdir(parents=True)
        (self.tmp / "libs" / "iris" / "src").mkdir(parents=True)
        (self.tmp / "libs" / "flux" / "src" / "demo.c").write_text(
            'FLUX_OBSERVE_ACQUIRE(x, y, FLUX_LIFETIME_FRAME, k, o, 0, s);\n'
        )
        (self.tmp / "libs" / "iris" / "src" / "demo.c").write_text(
            'flux_observe_report_emit_process();\nclose(fd);\n'
        )
        self.fx.header("""
            FIXTURE_API void demo_release(void *p);
            FIXTURE_API int demo_create(void **out);
        """)
        self.fx.lanes("""
            [kind.demo]
            scope    = "device"
            lifetime = "context"
            lane     = "tests/demo/test_lane.c"
        """)
        self.fx.exempt("")
        self.assert_error_contains("raw close() in iris")

    def test_file_line_site_string_fails(self):
        (self.tmp / "libs" / "flux" / "src").mkdir(parents=True)
        (self.tmp / "libs" / "iris" / "src").mkdir(parents=True)
        (self.tmp / "libs" / "flux" / "src" / "demo.c").write_text(
            '#define S() __FILE__ ":" "1"\n'
            'FLUX_OBSERVE_ACQUIRE(x, y, FLUX_LIFETIME_FRAME, k, o, 0, S());\n'
        )
        (self.tmp / "libs" / "iris" / "src" / "demo.c").write_text(
            'flux_observe_report_emit_process();\n'
        )
        self.fx.header("""
            FIXTURE_API void demo_release(void *p);
            FIXTURE_API int demo_create(void **out);
        """)
        self.fx.lanes("""
            [kind.demo]
            scope    = "device"
            lifetime = "context"
            lane     = "tests/demo/test_lane.c"
        """)
        self.fx.exempt("")
        self.assert_error_contains("per-call-site __FILE__ site string")

    # -- capability consumers --------------------------------------------

    def _fixture_with_observe_header(self, capabilities: str):
        """Minimal tree with a real observe.h-shaped capability block."""
        inc = self.tmp / "libs" / "flux" / "include" / "flux"
        inc.mkdir(parents=True)
        (inc / "observe.h").write_text("typedef enum caps {\n" + capabilities + "\n} caps;\n")
        (self.tmp / "libs" / "flux" / "src").mkdir(parents=True)
        (self.tmp / "libs" / "iris" / "src").mkdir(parents=True)
        self.fx.header("""
            FIXTURE_API void demo_release(void *p);
            FIXTURE_API int demo_create(void **out);
        """)
        self.fx.lanes("""
            [kind.demo]
            scope    = "device"
            lifetime = "context"
            lane     = "tests/demo/test_lane.c"
        """)
        self.fx.exempt("")

    def test_declared_capability_without_consumer_fails(self):
        """The defect this check exists for: four capability bits shipped with
        two of them read by nothing."""
        self._fixture_with_observe_header(
            "    FLUX_OBSERVE_COUNTS = 1u << 0,\n"
            "    FLUX_OBSERVE_NAMES = 1u << 1,"
        )
        (self.tmp / "libs" / "flux" / "src" / "demo.c").write_text(
            "FLUX_OBSERVE_ACQUIRE(x, y, FLUX_LIFETIME_FRAME, k, o, 0, s);\n"
        )
        (self.tmp / "libs" / "iris" / "src" / "demo.c").write_text(
            "flux_observe_report_emit_process();\n"
        )
        self.assert_error_contains("FLUX_OBSERVE_NAMES: declared but nothing")

    def test_declared_capability_with_consumer_passes(self):
        self._fixture_with_observe_header(
            "    FLUX_OBSERVE_COUNTS = 1u << 0,\n"
            "    FLUX_OBSERVE_NAMES = 1u << 1,"
        )
        (self.tmp / "libs" / "flux" / "src" / "demo.c").write_text(
            "FLUX_OBSERVE_ACQUIRE(x, y, FLUX_LIFETIME_FRAME, k, o, 0, s);\n"
            "flux_observe_name_object(d, s, k, o);\n"
            "flux_vk_cmd_begin_label(d, cmd, l);\n"
        )
        (self.tmp / "libs" / "iris" / "src" / "demo.c").write_text(
            "flux_observe_report_emit_process();\n"
        )
        self.assertEqual(self.errors(), [])

    # -- declared-but-unused capabilities ---------------------------------

    def test_frame_lifetime_declared_but_unused_fails(self):
        """A lifetime nothing declares cannot be enforced.

        The real tree shipped this for a release cycle: the per-frame drain
        check existed, was tested, and had no kind to check because no call
        site declared FLUX_LIFETIME_FRAME.
        """
        (self.tmp / "libs" / "flux" / "src").mkdir(parents=True)
        (self.tmp / "libs" / "iris" / "src").mkdir(parents=True)
        (self.tmp / "libs" / "flux" / "src" / "demo.c").write_text(
            '/* no frame-lifetime kind here */\n'
        )
        (self.tmp / "libs" / "iris" / "src" / "demo.c").write_text(
            'flux_observe_report_emit_process();\n'
        )
        self.fx.header("""
            FIXTURE_API void demo_release(void *p);
            FIXTURE_API int demo_create(void **out);
        """)
        self.fx.lanes("""
            [kind.demo]
            scope    = "device"
            lifetime = "context"
            lane     = "tests/demo/test_lane.c"
        """)
        self.fx.exempt("")
        self.assert_error_contains("no kind is declared FLUX_LIFETIME_FRAME")

    def test_process_ledger_never_drained_fails(self):
        """The backend that owns process-scope fds must drain them."""
        (self.tmp / "libs" / "flux" / "src").mkdir(parents=True)
        (self.tmp / "libs" / "iris" / "src").mkdir(parents=True)
        (self.tmp / "libs" / "flux" / "src" / "demo.c").write_text(
            'FLUX_OBSERVE_ACQUIRE(x, y, FLUX_LIFETIME_FRAME, k, o, 0, s);\n'
        )
        (self.tmp / "libs" / "iris" / "src" / "demo.c").write_text(
            '/* no drain */\n'
        )
        self.fx.header("""
            FIXTURE_API void demo_release(void *p);
            FIXTURE_API int demo_create(void **out);
        """)
        self.fx.lanes("""
            [kind.demo]
            scope    = "device"
            lifetime = "context"
            lane     = "tests/demo/test_lane.c"
        """)
        self.fx.exempt("")
        self.assert_error_contains("process ledger is never drained")

    def test_attributed_build_is_not_flagged(self):
        """A build that *did* request attribution may carry the machinery."""
        build = self.tmp / "build-attributed"
        (build / "meson-info").mkdir(parents=True)
        (build / "libs" / "flux").mkdir(parents=True)
        (build / "meson-info" / "intro-buildoptions.json").write_text(
            '[{"name":"observe_attribution","value":true}]'
        )
        (build / "libs" / "flux" / "libflux.so.0.0.73").write_text("")

        fake_bin = self.tmp / "bin"
        fake_bin.mkdir()
        fake_strings = fake_bin / "strings"
        fake_strings.write_text('#!/bin/sh\necho "%s.%s.%s"\n')
        fake_strings.chmod(0o755)

        self.fx.header("""
            FIXTURE_API void demo_release(void *p);
            FIXTURE_API int demo_create(void **out);
        """)
        self.fx.lanes("""
            [kind.demo]
            scope    = "device"
            lifetime = "context"
            lane     = "tests/demo/test_lane.c"
        """)
        self.fx.exempt("")

        import os
        old_path = os.environ.get("PATH", "")
        os.environ["PATH"] = f"{fake_bin}:{old_path}"
        try:
            self.assertEqual(self.errors(), [])
        finally:
            os.environ["PATH"] = old_path


if __name__ == "__main__":
    unittest.main(verbosity=2)
