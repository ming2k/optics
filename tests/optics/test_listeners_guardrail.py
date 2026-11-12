#!/usr/bin/env python3
"""Negative tests for the Wayland listener-slot guardrail (ADR-0145).

A guardrail nobody has seen fail is indistinguishable from a guardrail that
cannot fail. Each case below constructs the exact condition the guardrail
exists to catch, in a throwaway tree, and requires it to report — including the
shapes the scanner refuses to guess at, and the host-independence property that
motivated pinning the contract.

Run: python3 tests/optics/test_listeners_guardrail.py
"""

import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT))

import tools.check_listeners as listeners  # noqa: E402

CONTRACT = """
corpus = "wayland-protocols test"

[interface.wl_pointer]
events = ["enter", "leave", "motion", "button"]

[interface.wp_presentation_feedback]
events = ["sync_output", "presented", "discarded"]
"""

COMPLETE = """
#include <wayland-client.h>
static const struct wl_pointer_listener pointer_listener = {
    .enter = enter_cb,
    .leave = leave_cb,
    .motion = motion_cb,
    .button = button_cb,
};
static const struct wp_presentation_feedback_listener feedback_listener = {
    .sync_output = sync_output_cb,
    .presented = presented_cb,
    .discarded = discarded_cb,
};
"""


class Fixture:
    """A minimal repo tree: one Wayland backend TU and the pinned contract."""

    def __init__(self, tmp: Path, source: str = COMPLETE, contract: str = CONTRACT):
        self.root = tmp
        (tmp / "libs" / "iris" / "src").mkdir(parents=True)
        (tmp / "tools" / "config").mkdir(parents=True)
        (tmp / "libs" / "iris" / "src" / "app_wayland.c").write_text(textwrap.dedent(source))
        if contract is not None:
            (tmp / "tools" / "config" / "wayland-listeners.toml").write_text(
                textwrap.dedent(contract)
            )

    def violations(self) -> list[str]:
        return listeners.check_listeners(self.root)


class Base(unittest.TestCase):
    def make(self, **kwargs) -> Fixture:
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(tmp, ignore_errors=True))
        return Fixture(tmp, **kwargs)

    def assertViolation(self, violations: list[str], needle: str) -> None:
        joined = "\n".join(violations)
        self.assertIn(needle, joined, f"expected {needle!r} in:\n{joined}")


class TestMissingSlot(Base):
    def test_complete_tables_pass(self):
        self.assertEqual(self.make().violations(), [])

    def test_null_slot_is_reported_with_file_line_and_var(self):
        # The 0.0.72 regression, verbatim.
        fixture = self.make(
            source="""
            static const struct wp_presentation_feedback_listener feedback_listener = {
                .presented = presented_cb,
                .discarded = discarded_cb,
            };
            """
        )
        violations = fixture.violations()
        self.assertViolation(violations, "sync_output")
        self.assertViolation(violations, "feedback_listener")
        self.assertViolation(violations, "app_wayland.c:")

    def test_commented_out_handler_does_not_count(self):
        fixture = self.make(
            source="""
            static const struct wl_pointer_listener pointer_listener = {
                .enter = enter_cb,
                .leave = leave_cb,
                .motion = motion_cb,
                /* .button = button_cb, */
            };
            """
        )
        self.assertViolation(fixture.violations(), "'button'")


class TestContractStaleness(Base):
    def test_unknown_slot_means_contract_is_stale(self):
        fixture = self.make(
            source="""
            static const struct wl_pointer_listener pointer_listener = {
                .enter = a, .leave = b, .motion = c, .button = d,
                .axis = e,
            };
            """
        )
        violations = fixture.violations()
        self.assertViolation(violations, "'axis'")
        self.assertViolation(violations, "fix listeners")

    def test_interface_absent_from_contract_is_reported(self):
        fixture = self.make(
            source="""
            static const struct wl_touch_listener touch_listener = {.down = a};
            """
        )
        self.assertViolation(fixture.violations(), "wl_touch")


class TestUnprovableShapes(Base):
    def test_mutable_table_is_refused(self):
        fixture = self.make(
            source="""
            static struct wl_pointer_listener pointer_listener = {
                .enter = a, .leave = b, .motion = c, .button = d,
            };
            """
        )
        self.assertViolation(fixture.violations(), "not const")

    def test_runtime_populated_table_is_refused(self):
        fixture = self.make(
            source="""
            static const struct wl_pointer_listener pointer_listener;
            """
        )
        self.assertViolation(fixture.violations(), "without an initializer")

    def test_array_of_tables_is_refused(self):
        fixture = self.make(
            source="""
            static const struct wl_pointer_listener pointers[2] = {
                {.enter = a, .leave = b, .motion = c, .button = d},
            };
            """
        )
        self.assertViolation(fixture.violations(), "array of")


class TestFailsClosed(Base):
    def test_backend_with_no_tables_is_a_broken_scanner_not_a_pass(self):
        fixture = self.make(source="int main(void){return 0;}\n")
        self.assertViolation(fixture.violations(), "scanner is broken")

    def test_missing_contract_is_reported(self):
        fixture = self.make(contract=None)
        self.assertViolation(fixture.violations(), "missing")

    def test_invalid_contract_toml_is_reported(self):
        fixture = self.make(contract="[interface.wl_pointer\nevents = [\n")
        self.assertViolation(fixture.violations(), "invalid TOML")

    def test_duplicate_event_in_contract_is_reported(self):
        fixture = self.make(
            contract="""
            [interface.wl_pointer]
            events = ["enter", "enter", "leave", "motion", "button"]
            """
        )
        self.assertViolation(fixture.violations(), "more than once")

    def test_tree_without_wayland_backend_is_skipped(self):
        tmp = Path(tempfile.mkdtemp())
        self.addCleanup(lambda: __import__("shutil").rmtree(tmp, ignore_errors=True))
        (tmp / "libs" / "flux" / "src").mkdir(parents=True)
        (tmp / "libs" / "flux" / "src" / "canvas.c").write_text("int x;\n")
        self.assertEqual(listeners.check_listeners(tmp), [])


class TestHostIndependence(Base):
    """The property that motivated pinning: no verdict may depend on the host."""

    def test_check_never_reads_the_installed_corpus(self):
        original = listeners.protocol_xml_roots
        listeners.protocol_xml_roots = lambda root: self.fail("check consulted the host corpus")
        self.addCleanup(setattr, listeners, "protocol_xml_roots", original)

        self.assertEqual(self.make().violations(), [])
        self.assertViolation(
            self.make(
                source="static const struct wl_pointer_listener pointer_listener = {};"
            ).violations(),
            "wl_pointer",
        )

    def test_regenerate_without_corpus_leaves_contract_untouched(self):
        fixture = self.make()
        before = (fixture.root / "tools" / "config" / "wayland-listeners.toml").read_text()
        original = listeners.protocol_xml_roots
        listeners.protocol_xml_roots = lambda root: ([], "wayland-protocols (absent)")
        self.addCleanup(setattr, listeners, "protocol_xml_roots", original)

        ok, out, _ = listeners.regenerate_contract(fixture.root)
        self.assertTrue(ok)
        self.assertIn("untouched", out)
        after = (fixture.root / "tools" / "config" / "wayland-listeners.toml").read_text()
        self.assertEqual(before, after)


class TestCIFixability(Base):
    def test_fix_regenerates_from_a_corpus(self):
        fixture = self.make()
        original_roots = listeners.protocol_xml_roots
        original_events = listeners.load_protocol_events
        listeners.protocol_xml_roots = lambda root: ([fixture.root], "wayland-protocols 9.9")
        listeners.load_protocol_events = lambda roots: (
            {
                "wl_pointer": ["enter", "leave", "motion", "button", "axis"],
                "wp_presentation_feedback": ["sync_output", "presented", "discarded"],
            },
            [],
        )
        self.addCleanup(setattr, listeners, "protocol_xml_roots", original_roots)
        self.addCleanup(setattr, listeners, "load_protocol_events", original_events)

        ok, out, err = listeners.regenerate_contract(fixture.root)
        self.assertTrue(ok, err)
        self.assertIn("wayland-protocols 9.9", out)
        text = (fixture.root / "tools" / "config" / "wayland-listeners.toml").read_text()
        self.assertIn('"axis"', text)
        # Regenerated contract now disagrees with the table: the check must say so.
        self.assertViolation(fixture.violations(), "'axis'")

    def test_fix_refuses_an_interface_the_corpus_does_not_declare(self):
        fixture = self.make(
            source="""
            static const struct wl_nonexistent_listener strange_listener = {.evt = a};
            """
        )
        original_roots = listeners.protocol_xml_roots
        original_events = listeners.load_protocol_events
        listeners.protocol_xml_roots = lambda root: ([fixture.root], "wayland-protocols 9.9")
        listeners.load_protocol_events = lambda roots: ({"wl_pointer": ["enter"]}, [])
        self.addCleanup(setattr, listeners, "protocol_xml_roots", original_roots)
        self.addCleanup(setattr, listeners, "load_protocol_events", original_events)

        ok, _, err = listeners.regenerate_contract(fixture.root)
        self.assertFalse(ok)
        self.assertIn("wl_nonexistent", err)


if __name__ == "__main__":
    unittest.main(verbosity=2)
