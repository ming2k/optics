#!/usr/bin/env python3
"""Wayland listener-slot contract guardrail.

Answers one question mechanically: **can any protocol event reach a NULL slot in
a `wl_proxy` listener table?**

Why this is a guardrail and not a compiler diagnostic: every listener table is a
designated-initializer aggregate, and GCC and Clang *both* suppress
`-Wmissing-field-initializers` / `-Wmissing-designated-field-initializers` for
designated init (verified on gcc 16.1 and clang 22.1 — the aggregate arity hole
is invisible), while `sizeof` cannot express "every event of an interface
defined in someone else's header". The toolchain has no way to state this rule,
so it is stated here.

libwayland treats a NULL entry as unrecoverable. `wl_closure_invoke` calls
`wl_abort("listener function for opcode %u of %s is NULL")`, which `abort()`s
the process from inside `wl_display_dispatch`. There is no error return, no way
for a consumer to catch it, and the message names the protocol object rather
than the file that forgot the field:

    listener function for opcode 0 of wp_presentation_feedback is NULL
    Aborted (core dumped)

That exact abort shipped in Optics 0.0.72: `feedback_listener` implemented
`presented` and `discarded` but not `sync_output`, and niri sends
`sync_output` before every `presented`. The window died on its first
presented frame.

Coverage rule: **every event of the interface must have a slot.** A weaker
rule — "only events up to the bound version" — would pass today and crash
tomorrow the moment someone raises a `wl_registry_bind` version or a compositor
answers a loosely-versioned proxy. An event that can never be delivered still
costs three lines to stub, so there is no case where leaving the slot NULL is
worth the abort.

Expected event lists are **pinned** in `tools/config/wayland-listeners.toml`,
never read from the installed protocol corpus at check time:

  1. Determinism.  A verdict that depends on the host's `wayland-protocols`
     fails on macOS, on Windows, and on any image without the package —
     developer and CI disagreeing about a tree nobody changed.
  2. Reviewable drift.  A dependency bump that adds an event to an interface we
     already consume shows up as a one-line diff in a tracked file, reviewed
     like any other change, instead of red CI on an unrelated pull request.
  3. Fail-closed coverage.  A table for an interface the contract does not list
     is an error, so a new protocol adoption cannot silently escape
     verification by not being in the corpus yet.

`./tools/gate fix listeners` regenerates the contract from the local corpus
and records the corpus version it read, so staleness is visible in the diff.
The check itself needs no Wayland installation.

Everything this scanner cannot *prove* is an error, not a pass: a table that is
not `const`, a table declared without an initializer (populated at runtime), an
array of tables, and a tree with a Wayland backend but zero tables found (which
is how a broken parser would present itself).

Sunset: `libs/iris` is the only consumer. This guardrail, its contract file, and
its test are deleted in the same commit that removes the Wayland backend — the
contract exists to protect a backend, not to outlive it.
"""

from __future__ import annotations

import re
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

try:
    import tomllib  # stdlib since 3.11; the tooling floor is documented in tools/README.md
except ModuleNotFoundError:  # pragma: no cover - environment, not tree state
    sys.stderr.write(
        "error: the Wayland listener guardrail needs Python 3.11+ (stdlib tomllib).\n"
        f"       running: {sys.version.split()[0]}\n"
    )
    raise SystemExit(2) from None

CONTRACT_REL = Path("tools/config/wayland-listeners.toml")
BACKEND_DIR = Path("libs/iris/src")

C_COMMENT_RE = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)

# `static const struct xdg_toplevel_listener toplevel_listener = { ... };`
TABLE_RE = re.compile(
    r"(?P<mods>(?:\b(?:static|const|volatile|inline)\s+)*)"
    r"\bstruct\s+(?P<iface>\w+)_listener\s+(?P<var>\w+)\s*=\s*\{(?P<body>[^{}]*)\}",
    re.DOTALL,
)
# Declaration without an initializer: slots would be filled at runtime.
UNINITIALIZED_RE = re.compile(r"\bstruct\s+(?P<iface>\w+)_listener\s+(?P<var>\w+)\s*;")
# Array of tables: the aggregate body nests braces, so the flat scanner can
# never see it. Refusing beats silently skipping.
ARRAY_RE = re.compile(r"\bstruct\s+(?P<iface>\w+)_listener\s+(?P<var>\w+)\s*\[")
FIELD_RE = re.compile(r"\.(?P<field>\w+)\s*=")

TARGET_EXTS = {".c"}
SKIP_DIR_NAMES = {".git", "subprojects", "vendor", "target", "node_modules"}


@dataclass(frozen=True)
class ListenerTable:
    """One `const struct <iface>_listener <var> = {...}` in the tree."""

    path: str
    line: int
    iface: str
    var: str
    is_const: bool
    slots: tuple[str, ...]

    @property
    def struct(self) -> str:
        return f"{self.iface}_listener"


def strip_c_comments(source: str) -> str:
    """Blank C comments, preserving line count so reported lines stay true."""

    def replacer(m: re.Match[str]) -> str:
        return "\n" * m.group(0).count("\n")

    return C_COMMENT_RE.sub(replacer, source)


def iter_translation_units(repo_root: Path):
    for path in sorted(repo_root.rglob("*.c")):
        if path.suffix not in TARGET_EXTS:
            continue
        rel_parts = path.relative_to(repo_root).parts
        if any(part in SKIP_DIR_NAMES or part.startswith("build") for part in rel_parts[:-1]):
            continue
        yield path


def scan_tables(repo_root: Path) -> tuple[list[ListenerTable], list[str]]:
    """Every listener table in the tree, plus every shape it cannot prove."""
    tables: list[ListenerTable] = []
    unprovable: list[str] = []

    for path in iter_translation_units(repo_root):
        rel = path.relative_to(repo_root).as_posix()
        try:
            content = strip_c_comments(path.read_text(encoding="utf-8"))
        except OSError as exc:
            unprovable.append(f"{rel}: cannot read file: {exc}")
            continue

        for m in ARRAY_RE.finditer(content):
            line = content.count("\n", 0, m.start()) + 1
            unprovable.append(
                f"{rel}:{line}: {m.group('var')} is an array of "
                f"{m.group('iface')}_listener tables — the flat scanner cannot see "
                "nested aggregate bodies, so coverage cannot be proven"
            )

        for m in TABLE_RE.finditer(content):
            line = content.count("\n", 0, m.start()) + 1
            mods = m.group("mods").split()
            tables.append(
                ListenerTable(
                    path=rel,
                    line=line,
                    iface=m.group("iface"),
                    var=m.group("var"),
                    is_const="const" in mods,
                    slots=tuple(FIELD_RE.findall(m.group("body"))),
                )
            )

        for m in UNINITIALIZED_RE.finditer(content):
            line = content.count("\n", 0, m.start()) + 1
            unprovable.append(
                f"{rel}:{line}: {m.group('var')} is declared without an "
                f"initializer ({m.group('iface')}_listener) — a table whose slots "
                "are assigned at runtime cannot be verified statically; initialize "
                "it in one const aggregate instead"
            )

    return tables, unprovable


def protocol_xml_roots(repo_root: Path) -> tuple[list[Path], str]:
    """Directories holding the protocol XMLs this tree builds against.

    Generation-only. `check` never calls this: its verdict must not depend on
    what happens to be installed on the machine running it.
    """
    roots: list[Path] = []

    for lib in ("iris", "lens", "flux"):
        proto = repo_root / "libs" / lib / "protocol"
        if proto.is_dir():
            roots.append(proto)

    datadir = None
    try:
        res = subprocess.run(
            ["pkg-config", "--variable=pkgdatadir", "wayland-protocols"],
            capture_output=True,
            text=True,
            check=False,
        )
        if res.returncode == 0 and res.stdout.strip():
            datadir = Path(res.stdout.strip())
    except OSError:
        datadir = None

    for candidate in filter(
        None,
        (
            datadir,
            Path("/usr/share/wayland-protocols"),
            Path("/usr/local/share/wayland-protocols"),
        ),
    ):
        if candidate.is_dir():
            roots.append(candidate)
            break

    core = Path("/usr/share/wayland/wayland.xml")
    if core.is_file():
        roots.append(core)

    return roots, describe_corpus()


def describe_corpus() -> str:
    """Provenance string recorded in the contract header."""
    try:
        res = subprocess.run(
            ["pkg-config", "--modversion", "wayland-protocols"],
            capture_output=True,
            text=True,
            check=False,
        )
        if res.returncode == 0 and res.stdout.strip():
            return f"wayland-protocols {res.stdout.strip()}"
    except OSError:
        pass
    return "wayland-protocols (version unknown)"


def load_protocol_events(roots: list[Path]) -> tuple[dict[str, list[str]], list[str]]:
    """interface name -> ordered event names, plus the XMLs that failed.

    Interface names are not unique across the protocol corpus: the legacy
    `unstable/xdg-shell/xdg-shell-unstable-v5.xml` declares an `xdg_surface`
    with events `configure` *and* `close`, while the stable `xdg-shell`
    declares only `configure`. Resolution mirrors `wl.find_protocol`: local
    `protocol/` overlays first (they are the copies the scanner consumes), then
    stable/staging, then unstable newest-version-first. First definition of an
    interface wins.
    """
    events: dict[str, list[str]] = {}
    broken: list[str] = []

    def rank(xml: Path) -> tuple[int, int, str]:
        parts = xml.parts
        if "protocol" in parts:
            tier = 0
        elif "stable" in parts or "staging" in parts:
            tier = 1
        elif "unstable" in parts:
            tier = 2
        else:
            tier = 1  # wayland.xml core
        version = re.search(r"-v(\d+)\.xml$", xml.name)
        return (tier, -int(version.group(1)) if version else 0, str(xml))

    xmls: list[Path] = []
    for root in roots:
        xmls.extend([root] if root.is_file() else sorted(root.rglob("*.xml")))

    import xml.etree.ElementTree as ET

    for xml in sorted(xmls, key=rank):
        try:
            tree = ET.parse(xml)
        except (ET.ParseError, OSError) as exc:
            broken.append(f"{xml}: {exc}")
            continue
        for iface in tree.getroot().iter("interface"):
            name = iface.get("name")
            if not name or name in events:
                continue
            events[name] = [e.get("name") for e in iface.findall("event") if e.get("name")]
    return events, broken


def render_contract(events_by_iface: dict[str, list[str]], corpus: str) -> str:
    lines = [
        "# wayland-listeners.toml — the pinned event contract for every Wayland",
        "# listener table in libs/iris (INV-ARCH-WLSLOT).",
        "#",
        "# Why pinned instead of read from the installed wayland-protocols at check",
        "# time: a verdict that depends on the host either fails where the package is",
        "# absent (macOS, Windows, minimal images) or goes red on unrelated pull",
        "# requests when a dependency bump adds an event upstream. Pinned, an",
        "# upstream addition is a one-line diff, reviewed like any other change.",
        "#",
        "# An entry lists EVERY event the interface declares, not only those below",
        "# the version this tree binds: a slot left NULL passes today and aborts the",
        "# process the moment a bind version rises.",
        "#",
        "# Regenerate with:  ./tools/gate fix listeners",
        "# Review the diff. A new event here means a new slot is required in the C",
        "# table — libwayland abort()s on an unimplemented event.",
        "",
        f'corpus = "{corpus}"',
        "",
    ]
    for iface in sorted(events_by_iface):
        rendered = ", ".join(f'"{e}"' for e in events_by_iface[iface])
        lines += [f"[interface.{iface}]", f"events = [{rendered}]", ""]
    return "\n".join(lines)


def load_contract(repo_root: Path) -> tuple[dict[str, list[str]], list[str]]:
    path = repo_root / CONTRACT_REL
    if not path.is_file():
        return {}, [
            f"{CONTRACT_REL.as_posix()}: missing — regenerate with"
            " './tools/gate fix listeners'"
        ]
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as exc:
        return {}, [f"{CONTRACT_REL.as_posix()}: invalid TOML: {exc}"]

    errors: list[str] = []
    contract: dict[str, list[str]] = {}
    interfaces = data.get("interface")
    if not isinstance(interfaces, dict) or not interfaces:
        return {}, [
            f"{CONTRACT_REL.as_posix()}: declares no [interface.*] entries —"
            " regenerate with './tools/gate fix listeners'"
        ]
    for iface, spec in interfaces.items():
        events = spec.get("events") if isinstance(spec, dict) else None
        if not isinstance(events, list) or not events:
            errors.append(f"{CONTRACT_REL.as_posix()}: [interface.{iface}] has no events list")
            continue
        dupes = sorted({e for e in events if events.count(e) > 1})
        if dupes:
            errors.append(
                f"{CONTRACT_REL.as_posix()}: [interface.{iface}] lists "
                f"{', '.join(dupes)} more than once"
            )
            continue
        contract[iface] = list(events)
    return contract, errors


def check_listeners(repo_root: Path) -> list[str]:
    """Every way a protocol event can reach a NULL listener slot."""
    if not (repo_root / BACKEND_DIR).is_dir():
        return []  # No Wayland backend in this tree; nothing to protect.

    contract, errors = load_contract(repo_root)
    if errors:
        return errors

    tables, unprovable = scan_tables(repo_root)
    violations = list(unprovable)

    if not tables:
        # A parser that stopped matching would otherwise report a clean tree
        # forever. Silence is not evidence of compliance.
        return violations + [
            f"{BACKEND_DIR.as_posix()}: Wayland backend present but no listener"
            " table was found — the scanner is broken, not the tree"
        ]

    for table in tables:
        if not table.is_const:
            violations.append(
                f"{table.path}:{table.line}: {table.var} ({table.struct}) is not"
                " const — a writable listener table can have a slot nulled at"
                " runtime, and libwayland abort()s on the next such event"
            )
            continue

        events = contract.get(table.iface)
        if events is None:
            violations.append(
                f"{table.path}:{table.line}: {table.var} declares"
                f" {table.struct} for interface '{table.iface}', which is not in"
                f" {CONTRACT_REL.as_posix()} — regenerate with"
                " './tools/gate fix listeners' and review the added events"
            )
            continue

        missing = [event for event in events if event not in table.slots]
        if missing:
            violations.append(
                f"{table.path}:{table.line}: {table.var} ({table.struct}) has no"
                " slot for " + ", ".join(f"'{m}'" for m in missing)
            )

        unknown = [slot for slot in table.slots if slot not in events]
        if unknown:
            violations.append(
                f"{table.path}:{table.line}: {table.var} ({table.struct}) fills "
                + ", ".join(f"'{slot}'" for slot in unknown)
                + f", which {CONTRACT_REL.as_posix()} does not list as an event of"
                f" '{table.iface}' — the contract is stale; regenerate with"
                " './tools/gate fix listeners'"
            )

    return violations


def regenerate_contract(repo_root: Path) -> tuple[bool, str, str]:
    """Rewrite the pinned contract from the local protocol corpus."""
    roots, corpus = protocol_xml_roots(repo_root)
    if not roots:
        return (
            True,
            "wayland-listeners.toml left untouched (no protocol corpus installed:"
            " install wayland-protocols to regenerate)",
            "",
        )

    events, broken = load_protocol_events(roots)
    if not events:
        detail = "\n".join(f"  {b}" for b in broken) if broken else "  (no interfaces parsed)"
        return False, "", "error: Wayland protocol XML contained no interfaces:\n" + detail

    tables, _ = scan_tables(repo_root)
    if not tables:
        return (
            True,
            "wayland-listeners.toml left untouched (no listener tables in tree)",
            "",
        )

    used = sorted({table.iface for table in tables})
    unresolved = [iface for iface in used if iface not in events]
    if unresolved:
        return (
            False,
            "",
            "error: no protocol XML declares these listener interfaces, so the"
            " contract would be incomplete — install the matching"
            " wayland-protocols:\n"
            + "\n".join(f"  {iface}" for iface in unresolved)
            + "\n",
        )

    text = render_contract({iface: events[iface] for iface in used}, corpus)
    path = repo_root / CONTRACT_REL
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return True, (
        f"wrote {CONTRACT_REL.as_posix()} ({len(used)} interfaces, {corpus})"
    ), ""


def run_listener_check(repo_root: Path, fix: bool = False) -> tuple[bool, str, str]:
    if fix:
        return regenerate_contract(repo_root)

    violations = check_listeners(repo_root)
    if not violations:
        return True, "Wayland listener coverage verified (every interface event has a slot)", ""

    err = (
        "error: Wayland listener contract violated — libwayland calls abort() from"
        " inside wl_display_dispatch when it dispatches an event whose slot is"
        " NULL, and no consumer can catch that:\n"
        + "\n".join(f"  {v}" for v in violations)
        + "\n  Every event of the interface needs a handler (a no-op stub is fine).\n"
    )
    return False, "", err


def main() -> int:
    args = [a for a in sys.argv[1:] if a != "--fix"]
    root = Path(args[0]).resolve() if args else Path(__file__).resolve().parent.parent
    ok, out, err = run_listener_check(root, fix="--fix" in sys.argv[1:])
    if not ok:
        sys.stderr.write(err)
        return 1
    if out:
        print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
