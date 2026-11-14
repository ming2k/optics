"""Negative tests for the test-architecture guardrail.

The guardrail is itself a test gate. If it silently accepts an unclassified
test, a duplicate name, or an E2E source that reaches into library internals,
the taxonomy is decorative. These cases pin the failure paths without needing a
real Meson build tree.
"""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import pathlib
import tempfile
import types
import unittest
from unittest import mock


def load_guardrail():
    path = pathlib.Path(__file__).resolve().parent.parent.parent / "tools" / "check_test_layers.py"
    spec = importlib.util.spec_from_file_location("check_test_layers", path)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class LayerGuardrailTest(unittest.TestCase):
    def setUp(self):
        self.guardrail = load_guardrail()

    def run_introspection(self, entries):
        completed = types.SimpleNamespace(stdout=json.dumps(entries))
        with mock.patch.object(self.guardrail.subprocess, "run", return_value=completed):
            with contextlib.redirect_stdout(io.StringIO()):
                return self.guardrail.check_introspection(pathlib.Path("/tmp/optics-build"))

    def test_accepts_a_complete_pyramid(self):
        errors = self.run_introspection(
            [
                {"name": "one", "suite": ["optics:unit"]},
                {"name": "two", "suite": ["optics:integration"]},
                {"name": "three", "suite": ["optics:e2e"]},
                {"name": "bench_one", "suite": ["optics:bench"]},
                {"name": "lint_one", "suite": ["optics:lint"]},
            ]
        )
        self.assertEqual(errors, [])

    def test_rejects_an_unclassified_test(self):
        errors = self.run_introspection(
            [
                {"name": "one", "suite": ["optics:unit"]},
                {"name": "two", "suite": ["optics:integration"]},
                {"name": "three", "suite": ["optics:e2e"]},
                {"name": "orphan", "suite": ["optics"]},
            ]
        )
        self.assertTrue(any("orphan" in error for error in errors))

    def test_rejects_a_duplicate_name(self):
        errors = self.run_introspection(
            [
                {"name": "same", "suite": ["optics:unit"]},
                {"name": "same", "suite": ["optics:unit"]},
                {"name": "two", "suite": ["optics:integration"]},
                {"name": "three", "suite": ["optics:e2e"]},
            ]
        )
        self.assertTrue(any("same" in error for error in errors))

    def test_rejects_a_missing_e2e_layer(self):
        errors = self.run_introspection(
            [
                {"name": "one", "suite": ["optics:unit"]},
                {"name": "two", "suite": ["optics:integration"]},
            ]
        )
        self.assertTrue(any("e2e" in error for error in errors))

    def test_rejects_a_layer_and_lane_mix(self):
        errors = self.run_introspection(
            [
                {"name": "one", "suite": ["optics:unit"]},
                {"name": "two", "suite": ["optics:integration"]},
                {"name": "three", "suite": ["optics:e2e"]},
                {"name": "mixed", "suite": ["optics:unit", "optics:bench"]},
            ]
        )
        self.assertTrue(any("mixed" in error for error in errors))

    def test_rejects_private_harnesses_and_e2e_internals(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "tests" / "lens").mkdir(parents=True)
            (root / "tests" / "lens" / "test_helpers.h").write_text("")
            e2e = root / "tests" / "e2e"
            e2e.mkdir()
            (e2e / "bad.c").write_text('#include "../../libs/lens/src/internal.h"\n')
            (e2e / "meson.build").write_text("sources = ['../../libs/lens/src/x.c']\n")

            errors = self.guardrail.check_no_private_harness(root)
            errors += self.guardrail.check_e2e_boundary(root)

        self.assertTrue(any("private assertion harness" in error for error in errors))
        self.assertTrue(any("private header" in error for error in errors))
        self.assertTrue(any("compiles library sources" in error for error in errors))

    def test_rejects_device_work_in_a_unit_suite(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            unit = root / "tests" / "flux" / "unit"
            unit.mkdir(parents=True)
            (unit / "bad.c").write_text("int main(void) { flux_device_create(); }\n")
            errors = self.guardrail.check_unit_cpu_purity(root)
        self.assertTrue(any("move it to integration" in error for error in errors))

    def test_rejects_legacy_harness_tokens(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / "tests" / "lens").mkdir(parents=True)
            (root / "tests" / "lens" / "bad.c").write_text("#define CHECK(x) x\n")
            errors = self.guardrail.check_no_private_harness(root)
        self.assertTrue(any("legacy/private assertion token" in error for error in errors))

    def test_rejects_unregistered_e2e_source(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            e2e = root / "tests" / "e2e"
            e2e.mkdir(parents=True)
            (e2e / "meson.build").write_text("test('one', executable('one', 'one.c'))\n")
            (e2e / "orphan.c").write_text("int main(void) { return 0; }\n")
            errors = self.guardrail.check_e2e_registration(root)
        self.assertTrue(any("not registered" in error for error in errors))


if __name__ == "__main__":
    unittest.main()
