#!/usr/bin/env python3
"""Optics Developer Tooling & Guardrails Unified CLI.

Provides single-entry architecture governance, verification, and automated remediation
for the Optics monorepo (ADR-0098, ADR-0125).

Usage:
  ./tools/optics check [name]  # Run all guardrails or a specific one (format, boundary, lockstep, stdout, symbols, docs, tidy)
  ./tools/optics fix           # Auto-format C sources and regenerate symbols.md
  ./tools/optics list          # List available guardrails, domains, and policy mapping
"""

import argparse
import sys
import time
from pathlib import Path

# ANSI color codes
GREEN = "\033[32m"
RED = "\033[31m"
YELLOW = "\033[33m"
CYAN = "\033[36m"
BOLD = "\033[1m"
RESET = "\033[0m"

if not sys.stdout.isatty():
    GREEN = RED = YELLOW = CYAN = BOLD = RESET = ""


def get_repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def run_format(repo_root: Path, fix: bool, **kwargs) -> tuple[bool, str, str]:
    from tools.guardrails.formatting import run_format_check
    return run_format_check(repo_root, fix=fix)


def run_boundary(repo_root: Path, fix: bool, **kwargs) -> tuple[bool, str, str]:
    from tools.guardrails.boundary import check_boundary
    violations = check_boundary(repo_root)
    if violations:
        err = "error: material boundary violation — flux references a material name:\n" + "\n".join(violations) + "\n"
        return False, "", err
    return True, "material boundary separation verified", ""


def run_lockstep(repo_root: Path, fix: bool, **kwargs) -> tuple[bool, str, str]:
    from tools.guardrails.lockstep import check_lockstep, get_project_version
    errors = check_lockstep(repo_root)
    if errors:
        err = "\n".join(errors) + "\n"
        return False, "", err
    major, minor, patch = get_project_version(repo_root)
    return True, f"version lockstep OK ({major}.{minor}.{patch})", ""


def run_stdout_purity(repo_root: Path, fix: bool, **kwargs) -> tuple[bool, str, str]:
    from tools.guardrails.stdout_purity import check_stdout_purity
    violations = check_stdout_purity(repo_root)
    if violations:
        err = "error: library source writes to stdout — diagnostics belong on stderr:\n" + "\n".join(violations) + "\n"
        return False, "", err
    return True, "library stdout purity verified", ""


def run_symbols(repo_root: Path, fix: bool, **kwargs) -> tuple[bool, str, str]:
    from tools.guardrails.symbols import run_symbols_check
    return run_symbols_check(repo_root, check_only=not fix)


def run_doc_links(repo_root: Path, fix: bool, **kwargs) -> tuple[bool, str, str]:
    from tools.guardrails.doc_links import check_documentation
    problems = check_documentation(str(repo_root))
    if problems:
        err = f"error: documentation governance failed with {len(problems)} violation(s):\n" + "\n".join(f"  ERROR: {p}" for p in problems) + "\n"
        return False, "", err
    return True, "documentation integrity, hyperlinks, audience firewall, and ADR lineage verified", ""


def run_tidy_gate(repo_root: Path, fix: bool, **kwargs) -> tuple[bool, str, str]:
    from tools.guardrails.tidy import run_tidy
    build_dir = kwargs.get("build_dir") or "build-tidy"
    return run_tidy(repo_root, build_dir=build_dir)


QUALITY_GUARDRAILS = [
    {
        "id": "format",
        "name": "Code formatting (clang-format 22.x)",
        "policy": "ADR-0098, .clang-format",
        "runner": run_format,
        "supports_fix": True,
    },
    {
        "id": "boundary",
        "name": "Material boundary separation",
        "policy": "ADR-0063 (Layer 0 flux isolation)",
        "runner": run_boundary,
        "supports_fix": False,
    },
    {
        "id": "lockstep",
        "name": "Version lockstep across monorepo",
        "policy": "Monorepo lockstep (ADR-0098)",
        "runner": run_lockstep,
        "supports_fix": False,
    },
    {
        "id": "stdout",
        "name": "Library stdout purity (stderr only)",
        "policy": "Library IPC hygiene",
        "runner": run_stdout_purity,
        "supports_fix": False,
    },
    {
        "id": "symbols",
        "name": "Public symbols reference freshness",
        "policy": "ADR-0098, docs/reference/symbols.md",
        "runner": run_symbols,
        "supports_fix": True,
    },
    {
        "id": "docs",
        "name": "Documentation integrity & audience firewall",
        "policy": "ADR-0125",
        "runner": run_doc_links,
        "supports_fix": False,
    },
]

EXTRA_GUARDRAILS = [
    {
        "id": "tidy",
        "name": "Clang-tidy static analysis",
        "policy": "Zero-warning baseline",
        "runner": run_tidy_gate,
        "supports_fix": False,
    },
]

ALL_GUARDRAILS = QUALITY_GUARDRAILS + EXTRA_GUARDRAILS


def cmd_check(args: argparse.Namespace) -> int:
    repo_root = get_repo_root()
    if str(repo_root) not in sys.path:
        sys.path.insert(0, str(repo_root))

    target = args.target.lower() if args.target else "all"
    fix = bool(args.fix)

    if target != "all":
        selected = [g for g in ALL_GUARDRAILS if g["id"] == target]
        if not selected:
            valid = ", ".join(g["id"] for g in ALL_GUARDRAILS)
            sys.stderr.write(f"Unknown guardrail target: '{target}'. Valid targets: all, {valid}\n")
            return 1
    else:
        selected = QUALITY_GUARDRAILS

    total = len(selected)
    print(f"{BOLD}=== Optics Architecture & Quality Guardrails ==={RESET}")
    start_all = time.perf_counter()

    for idx, g in enumerate(selected, start=1):
        label = f"[{idx}/{total}] {g['name']}..."
        sys.stdout.write(f"{label:<55} ")
        sys.stdout.flush()

        t0 = time.perf_counter()
        try:
            ok, stdout_msg, stderr_msg = g["runner"](repo_root, fix, build_dir=getattr(args, "build_dir", None))
        except Exception as e:
            ok, stdout_msg, stderr_msg = False, "", str(e)
        elapsed_ms = (time.perf_counter() - t0) * 1000

        if ok:
            if fix and g["supports_fix"]:
                status = f"{GREEN}fixed{RESET}"
            else:
                status = f"{GREEN}OK{RESET}"
            print(f"{status} {CYAN}[{elapsed_ms:.0f}ms]{RESET}")
        else:
            print(f"{RED}FAIL{RESET} {CYAN}[{elapsed_ms:.0f}ms]{RESET}")
            if stderr_msg:
                sys.stderr.write(stderr_msg + "\n")
            elif stdout_msg:
                sys.stderr.write(stdout_msg + "\n")
            if not fix and g["supports_fix"]:
                sys.stderr.write(f"\n{YELLOW}hint: run './tools/optics fix' to automatically remediate.{RESET}\n")
            return 1

    total_ms = (time.perf_counter() - start_all) * 1000
    print(f"{GREEN}{BOLD}All guardrails passed: tree is clean and compliant.{RESET} {CYAN}[total: {total_ms:.0f}ms]{RESET}")
    return 0


def cmd_fix(args: argparse.Namespace) -> int:
    args.fix = True
    args.target = "all"
    return cmd_check(args)


def cmd_list(args: argparse.Namespace) -> int:
    print(f"{BOLD}Optics Guardrails & Developer Tooling Catalog{RESET}\n")
    print(f"{'Target':<12} {'Description':<50} {'Policy / Invariant':<30}")
    print("-" * 94)
    for g in ALL_GUARDRAILS:
        fixable = " (auto-fixable)" if g["supports_fix"] else ""
        print(f"{g['id']:<12} {g['name'] + fixable:<50} {g['policy']:<30}")
    print("\nRun './tools/optics check' to verify core quality guardrails.")
    print("Run './tools/optics check tidy' to run clang-tidy static analysis.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        prog="optics",
        description="Unified developer tooling and architecture guardrail CLI for Optics.",
    )
    subparsers = parser.add_subparsers(dest="subcommand")

    # check
    p_check = subparsers.add_parser("check", help="Run architecture guardrails")
    p_check.add_argument(
        "target",
        nargs="?",
        default="all",
        help="Specific guardrail (format, boundary, lockstep, stdout, symbols, docs, tidy; default: all)",
    )
    p_check.add_argument("--fix", action="store_true", help="Auto-fix format and symbol parity")
    p_check.add_argument("--build-dir", default=None, help="Build directory for tidy/compile checks")
    p_check.set_defaults(func=cmd_check)

    # fix
    p_fix = subparsers.add_parser("fix", help="Automatically format sources and refresh symbols")
    p_fix.add_argument("--build-dir", default=None, help=argparse.SUPPRESS)
    p_fix.set_defaults(func=cmd_fix)

    # list
    p_list = subparsers.add_parser("list", help="List available guardrails and tool metadata")
    p_list.set_defaults(func=cmd_list)

    if len(sys.argv) == 1:
        args = parser.parse_args(["check"])
    else:
        args = parser.parse_args()

    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
