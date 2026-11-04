#!/usr/bin/env python3
"""Run automated headless Tracy profiling and assert industrial performance SLAs (ADR-0138, ADR-0141).

Mechanically verifies that hot-path execution across iris (mailbox), lens (layout/store),
glyph (text), prism (materials), and flux (present) satisfies strict latency budgets.
"""

from __future__ import annotations

import csv
import io
import os
import shutil
import subprocess
import time
from pathlib import Path


def ensure_tracy_tools(repo_root: Path) -> tuple[Path | None, Path | None, str | None]:
    """Locate or build tracy-capture and tracy-csvexport binaries."""
    capture_bin = shutil.which("tracy-capture") or shutil.which("capture")
    csvexport_bin = shutil.which("tracy-csvexport") or shutil.which("csvexport")

    candidate_dirs = [
        Path("/tmp/tracy-capture-build"),
        Path("/tmp/tracy-csvexport-build"),
        Path("/tmp/tracy-tools"),
    ]
    for d in candidate_dirs:
        if not capture_bin and (d / "tracy-capture").is_file():
            capture_bin = str(d / "tracy-capture")
        if not csvexport_bin and (d / "tracy-csvexport").is_file():
            csvexport_bin = str(d / "tracy-csvexport")

    if capture_bin and csvexport_bin:
        return Path(capture_bin), Path(csvexport_bin), None

    # Build from subprojects/tracy-0.14.1 if available
    tracy_src = repo_root / "subprojects" / "tracy-0.14.1"
    if not tracy_src.is_dir():
        return None, None, f"Tracy source tree not found at {tracy_src}"

    tools_dir = Path("/tmp/tracy-tools")
    tools_dir.mkdir(parents=True, exist_ok=True)

    if not capture_bin:
        cap_build = Path("/tmp/tracy-capture-build")
        res = subprocess.run(
            ["cmake", "-B", str(cap_build), "-S", str(tracy_src / "capture"), "-DCMAKE_BUILD_TYPE=Release"],
            capture_output=True, text=True
        )
        if res.returncode != 0:
            return None, None, f"Failed to configure tracy-capture:\n{res.stderr}"
        res = subprocess.run(["cmake", "--build", str(cap_build), "-j2"], capture_output=True, text=True)
        if res.returncode != 0:
            return None, None, f"Failed to build tracy-capture:\n{res.stderr}"
        capture_bin = str(cap_build / "tracy-capture")

    if not csvexport_bin:
        csv_build = Path("/tmp/tracy-csvexport-build")
        res = subprocess.run(
            ["cmake", "-B", str(csv_build), "-S", str(tracy_src / "csvexport"), "-DCMAKE_BUILD_TYPE=Release"],
            capture_output=True, text=True
        )
        if res.returncode != 0:
            return None, None, f"Failed to configure tracy-csvexport:\n{res.stderr}"
        res = subprocess.run(["cmake", "--build", str(csv_build), "-j2"], capture_output=True, text=True)
        if res.returncode != 0:
            return None, None, f"Failed to build tracy-csvexport:\n{res.stderr}"
        csvexport_bin = str(csv_build / "tracy-csvexport")

    return Path(capture_bin), Path(csvexport_bin), None


def profile_workload(
    binary: Path,
    args: list[str],
    capture_bin: Path,
    csvexport_bin: Path,
    trace_path: Path,
    duration_s: float = 2.0,
) -> dict[str, dict[str, float]]:
    """Execute workload with TRACY_NO_EXIT=1, capture trace, and extract zone metrics."""
    if trace_path.is_file():
        trace_path.unlink()

    env = os.environ.copy()
    env["TRACY_NO_EXIT"] = "1"

    # Start workload process
    proc = subprocess.Popen([str(binary)] + args, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        # Give process brief moment to initialize tracy listen socket
        time.sleep(0.1)

        # Run capture
        cap_cmd = [str(capture_bin), "-o", str(trace_path), "-s", str(int(duration_s))]
        subprocess.run(cap_cmd, capture_output=True, text=True, timeout=duration_s + 5.0)
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=2.0)
        except Exception:
            proc.kill()

    if not trace_path.is_file() or trace_path.stat().st_size == 0:
        return {}

    # Extract CSV
    csv_res = subprocess.run([str(csvexport_bin), str(trace_path)], capture_output=True, text=True)
    if csv_res.returncode != 0:
        return {}

    zones: dict[str, dict[str, float]] = {}
    reader = csv.DictReader(io.StringIO(csv_res.stdout))
    for row in reader:
        name = row.get("name")
        if not name:
            continue
        try:
            zones[name] = {
                "counts": float(row.get("counts", 0)),
                "mean_ns": float(row.get("mean_ns", 0)),
                "min_ns": float(row.get("min_ns", 0)),
                "max_ns": float(row.get("max_ns", 0)),
            }
        except ValueError:
            continue

    return zones


def run_bench_sla(repo_root: Path, build_dir: str = "build", **kwargs) -> tuple[bool, str, str]:
    """Execute headless profiling and mechanically enforce latency SLA budgets."""
    b_path = repo_root / build_dir
    if not b_path.is_dir():
        return False, "", f"Build directory '{build_dir}' not found. Run 'meson setup {build_dir} -Dtracy=true' first."

    ninja_file = b_path / "build.ninja"
    if ninja_file.is_file() and "DTRACY_ENABLE" not in ninja_file.read_text(encoding="utf-8", errors="ignore"):
        return False, "", (
            f"Build directory '{build_dir}' has Tracy profiling disabled (-Dtracy=false).\n"
            f"Run 'meson configure {build_dir} -Dtracy=true && ninja -C {build_dir}' to enable SLA benchmarking."
        )

    capture_bin, csvexport_bin, err = ensure_tracy_tools(repo_root)
    if err:
        return False, "", f"Tracy headless toolchain setup failed: {err}"

    workloads = [
        {
            "name": "iris-mailbox",
            "binary": b_path / "tests" / "iris" / "test_frame_mailbox",
            "slas": {
                "iris.mailbox_publish": {"max_mean_ns": 1_000.0, "label": "Mailbox publish (wait-free)"},
                "iris.mailbox_claim": {"max_mean_ns": 1_000.0, "label": "Mailbox claim (wait-free)"},
            },
        },
        {
            "name": "lens-stress",
            "binary": b_path / "tests" / "lens" / "test_stress",
            "slas": {
                "lens.layout_solve": {"max_mean_ns": 100_000.0, "label": "Layout solve (500 nodes)"},
                "lens.store_reap": {"max_mean_ns": 50_000.0, "label": "Store GC reap"},
            },
        },
        {
            "name": "prism-liquid-glass",
            "binary": b_path / "tests" / "prism" / "integration" / "test_liquid_glass",
            "slas": {
                "prism.liquid_glass_apply": {"max_mean_ns": 1_000_000.0, "label": "Liquid glass dispatch"},
            },
        },
        {
            "name": "flux-frame-loop",
            "binary": b_path / "tests" / "flux" / "bench" / "bench_frame_loop",
            "slas": {
                "flux.frame_present": {"max_mean_ns": 20_000.0, "label": "Vulkan queue present submission"},
            },
        },
    ]

    trace_tmp = Path("/tmp/optics_sla_check.tracy")
    violations: list[str] = []
    summary_lines: list[str] = []

    for w in workloads:
        bin_path = w["binary"]
        if not bin_path.is_file():
            violations.append(f"Missing benchmark binary: {bin_path}")
            continue

        zones = profile_workload(bin_path, [], capture_bin, csvexport_bin, trace_tmp, duration_s=2.0)
        for zone_name, sla_spec in w["slas"].items():
            data = zones.get(zone_name)
            if not data:
                violations.append(f"[{w['name']}] Zone '{zone_name}' was not hit during profile run.")
                continue

            mean_ns = data["mean_ns"]
            max_ns = data["max_ns"]
            limit_ns = sla_spec["max_mean_ns"]
            label = sla_spec["label"]

            status = "PASS" if mean_ns <= limit_ns else "FAIL"
            summary_lines.append(
                f"  {label:<34} mean: {mean_ns/1000.0:8.2f} µs (limit: {limit_ns/1000.0:6.1f} µs) max: {max_ns/1000.0:8.2f} µs [{status}]"
            )

            if mean_ns > limit_ns:
                violations.append(
                    f"[{w['name']}] SLA VIOLATION: '{zone_name}' mean latency {mean_ns/1000.0:.2f} µs exceeded {limit_ns/1000.0:.1f} µs budget!"
                )

    if trace_tmp.is_file():
        trace_tmp.unlink()

    report = "Headless Tracy Performance SLA Matrix (ADR-0138, ADR-0141):\n" + "\n".join(summary_lines)
    if violations:
        err_msg = report + "\n\nSLA ENFORCEMENT FAILURES:\n" + "\n".join(f"  ERROR: {v}" for v in violations) + "\n"
        return False, "", err_msg

    return True, report, ""


if __name__ == "__main__":
    success, rep, err = run_bench_sla(Path(__file__).resolve().parent.parent.parent)
    if success:
        print(rep)
        print("\nAll performance SLAs PASSED.")
    else:
        print(err)
        exit(1)
