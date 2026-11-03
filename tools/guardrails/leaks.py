#!/usr/bin/env python3
"""Leak-coverage guardrail (ADR-0130, INV-ARCH-LEDGER).

Answers one question mechanically: **is every public resource kind covered by
a leak lane, or explicitly exempted with a written reason?**

The inventory is derived from the installed headers, so it cannot go stale when
a new resource type is added. Each kind must appear in
`tools/config/leak-lanes.toml` with a lane path that exists on disk and is
registered as a meson test (a test that never runs is not coverage), or in
`tools/config/leak-exempt.toml` with a reason and a substitute assertion.

Three independent checks:

  (a) coverage   — every kind has a lane or a reasoned exemption
  (b) attribution— a build without attribution must not carry the name/site
                   machinery (symbol absence, checked with nm if available)
  (c) hygiene    — the lane files exist, are registered, and the exemption
                   ledger has no entry without a reason

Syntax-aware, like boundary.py / stdout_purity.py: C comments are stripped
before scanning so a documented symbol is not mistaken for an exported one.
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

TARGET_EXTS = {".h"}
EXCLUDE_DIRS = {".git", "build", "build-dev", "build-release", "build-tidy",
                "subprojects", "vendor", "target"}
C_COMMENT_RE = re.compile(r"//[^\n]*|/\*.*?\*/", re.DOTALL)

# Public resource kinds are the acquire-side verbs. A `*_create` that has no
# matching release is a design defect the inventory surfaces for free.
KIND_RE = re.compile(
    r"\b(?P<module>[a-z][a-z0-9]*)"
    r"(?:_[a-z0-9_]+?)*?_"
    r"(?P<verb>create|retain|acquire)\b"
)
# Every release-side verb, used to prove a kind is releasable at all.
RELEASE_RE = re.compile(r"\b[a-z][a-z0-9_]*(?:_[a-z0-9_]+?)*?_(release|destroy|free)\b")

# Symbols that only exist when attribution is compiled in (ADR-0129
# decision 2). A default build must not export any of them.
# The attribution machinery leaves a signature in the object file that a
# build without it cannot contain: the format template used to derive an
# object's debug name from its ledger row. Checking for the template is
# stronger than checking for a symbol, because the symbol (the API entry
# point) exists in both builds by design — it is the *work* that must be
# absent, and that work drags the format string with it.
ATTRIBUTION_MARKER = "%s.%s.%s"

def strip_c_comments(source: str) -> str:
    def replacer(m):
        return "\n" * m.group(0).count("\n")

    return C_COMMENT_RE.sub(replacer, source)


def iter_headers(repo_root: Path):
    for lib in sorted((repo_root / "libs").iterdir()):
        inc = lib / "include"
        if not inc.is_dir():
            continue
        for path in sorted(inc.rglob("*.h")):
            if path.suffix in TARGET_EXTS:
                yield path


# Export macros follow one repo-wide convention: <MODULE>_API in all caps.
# Matching the convention rather than an enumerated list is deliberate — an
# enumerated list silently stops covering a new library, which is exactly the
# drift this guardrail exists to catch.
API_MARKER_RE = re.compile(r"\b[A-Z][A-Z0-9]*_API\b")

# Verbs that own a resource lifetime. `_acquire` is deliberately NOT here:
# symbols like flux_canvas_wait_dmabuf_acquire end in "acquire" but own
# nothing, and admitting them would fill the ledger with noise that makes the
# real gaps harder to see.
ACQUIRE_VERBS = re.compile(r"^(?P<base>[a-z][a-z0-9_]*?)_(create|retain)$")
RELEASE_VERBS = ("_release", "_destroy")


def collect_declarations(repo_root: Path) -> tuple[set[str], set[str]]:
    """Return (declared_public_symbols, declared_release_symbols).

    Declarations wrap across lines in the real headers —
    ``PRISM_NODISCARD PRISM_API`` on one line and the symbol on the next — so
    the scan runs over the comment-stripped *file*, anchored on an API marker
    and bounded by the next statement terminator. A per-line scan silently
    misses exactly the kinds whose declarations got reformatted, which is the
    same staleness this guardrail exists to prevent.
    """
    declared: set[str] = set()
    releases: set[str] = set()

    # API marker ... symbol( ... ;  — bounded by the statement terminator, so
    # one marker cannot claim a symbol from an unrelated later declaration.
    decl_re = re.compile(
        r"(?:\b[A-Z][A-Z0-9]*_API\b)[^;{}]*?\b([a-z][a-z0-9_]*)\s*\(",
        re.DOTALL,
    )

    for header in iter_headers(repo_root):
        try:
            text = strip_c_comments(header.read_text(encoding="utf-8"))
        except Exception as e:  # noqa: BLE001 - report, never crash the gate
            print(f"warning: {header}: cannot read: {e}", file=sys.stderr)
            continue
        for m in decl_re.finditer(text):
            sym = m.group(1)
            declared.add(sym)
            if sym.endswith(RELEASE_VERBS):
                releases.add(sym)
    return declared, releases


def collect_kinds(repo_root: Path) -> dict[str, dict]:
    """Resource kind -> {"owner": release symbol or None, "symbols": [...]}.

    A kind is the resource itself (`flux_image`), not each entry point that
    minted a reference to it. Keying on `flux_image_create` and
    `flux_image_retain` separately would demand two lanes for one lifetime and
    teach reviewers that the ledger is paperwork.

    Every public `*_create` / `*_retain` contributes to a kind. A kind whose
    release verb does not exist is not silently dropped: it becomes a kind
    that *requires* an exemption with a written reason, because "this is
    returned to the caller by another route" (an arena, a borrowed view) is a
    decision somebody has to state, not an absence a reader has to rediscover.
    """
    declared, releases = collect_declarations(repo_root)
    kinds: dict[str, dict] = {}
    for sym in sorted(declared):
        m = ACQUIRE_VERBS.match(sym)
        if not m:
            continue
        base = m.group("base")
        entry = kinds.setdefault(base, {"owner": None, "symbols": []})
        entry["symbols"].append(sym)
        for suffix in RELEASE_VERBS:
            if base + suffix in releases:
                entry["owner"] = base + suffix
                break
    return kinds


def parse_toml_ledger(path: Path) -> tuple[dict[str, dict], list[str]]:
    """Minimal TOML reader for the flat `[table.key]` shape used here.

    A full TOML dependency would be an out-of-tree build input; the ledgers are
    deliberately restricted to one shape so a small parser suffices. Multi-line
    triple-quoted values are supported because an exemption reason that has to
    fit on one line is an exemption reason that says nothing.
    Returns (entries, errors).
    """
    entries: dict[str, dict] = {}
    errors: list[str] = []
    if not path.is_file():
        errors.append(f"missing ledger: {path.relative_to(path.parents[2])}")
        return entries, errors

    current = None
    lines = path.read_text(encoding="utf-8").splitlines()
    i = 0
    while i < len(lines):
        line = lines[i].strip()
        i += 1
        if not line or line.startswith("#"):
            continue
        m = re.fullmatch(r"\[([A-Za-z0-9_.\-]+)\]", line)
        if m:
            current = m.group(1)
            entries[current] = {"_line": i}
            continue
        m = re.fullmatch(r'([A-Za-z0-9_\-]+)\s*=\s*"""(.*)', line)
        if m and current:
            key, rest = m.group(1), m.group(2)
            buf = []
            if rest.rstrip().endswith('"""'):
                buf.append(rest.rstrip()[:-3])
            else:
                buf.append(rest)
                while i < len(lines):
                    nxt = lines[i]
                    i += 1
                    if nxt.strip().endswith('"""'):
                        buf.append(nxt[: nxt.rindex('"""')])
                        break
                    buf.append(nxt)
            entries[current][key] = " ".join(x.strip() for x in buf).strip()
            continue
        m = re.fullmatch(r'([A-Za-z0-9_\-]+)\s*=\s*"([^"]*)"', line)
        if m and current:
            entries[current][m.group(1)] = m.group(2)
    return entries, errors


def registered_tests(repo_root: Path) -> set[str] | None:
    """Test source paths named by `test(...)` / `executable(...)` in meson
    files. Returns None when the tree has no meson files to parse (treated as
    "cannot verify", never as pass).

    Also resolves the `foreach` pattern the lens and prism suites use
    (`tests = ['test_store', ...]` / `foreach t : tests`), because a lane that
    points at a file registered through a loop is still registered — and a
    checker that cannot see that would force every lane to be rewritten to
    satisfy the checker instead of the other way round.
    """
    meson_files = list(repo_root.glob("tests/**/meson.build"))
    if not meson_files:
        return None
    names: set[str] = set()
    exe_re = re.compile(r"executable\(\s*'([a-z0-9_\-]+)'", re.IGNORECASE)
    test_re = re.compile(r"\btest\(\s*'([a-z0-9_\-]+)'", re.IGNORECASE)
    list_re = re.compile(r"^\s*[a-z_]+\s*=\s*\[(.*?)\]", re.IGNORECASE | re.DOTALL)
    str_re = re.compile(r"'([a-z0-9_\-]+)'")
    for mf in meson_files:
        text = mf.read_text(encoding="utf-8")
        for m in exe_re.finditer(text):
            names.add(m.group(1))
        for m in test_re.finditer(text):
            names.add(m.group(1))
        for m in list_re.finditer(text):
            for sm in str_re.finditer(m.group(1)):
                names.add(sm.group(1))
    return names


def _binary_markers(lib_path: Path) -> set[str] | None:
    """String literals present in a shared object, or None if unreadable."""
    strings = shutil.which("strings")
    if strings is None or not lib_path.is_file():
        return None
    try:
        out = subprocess.run([strings, str(lib_path)], capture_output=True, text=True,
                             timeout=120, check=False)
    except Exception:  # noqa: BLE001
        return None
    if out.returncode != 0:
        return None
    return set(out.stdout.splitlines())


def check_attribution(repo_root: Path, errors: list[str]) -> None:
    """A build without -Dobserve_attribution must not carry the attribution
    machinery (ADR-0129 decision 2).

    Inspects every configured build directory that does not request
    attribution. Silence when no such build exists would make this check
    unfalsifiable, so absence of a configured build is reported once as a
    note rather than passing quietly.
    """
    candidates = []
    for build_dir in sorted(repo_root.glob("build*")):
        opts = build_dir / "meson-info" / "intro-buildoptions.json"
        if not opts.is_file():
            continue
        try:
            text = opts.read_text(encoding="utf-8")
        except Exception:  # noqa: BLE001
            continue
        attributed = re.search(r'"observe_attribution"[^}]*?"value":\s*true', text) is not None
        if not attributed:
            candidates.append(build_dir)

    if not candidates:
        print("note: leaks/attribution not verified — no built library to inspect",
              file=sys.stderr)
        return

    inspected = 0
    for build_dir in candidates:
        lib = build_dir / "libs" / "flux" / "libflux.so.0.0.71"
        if not lib.is_file():
            found = sorted((build_dir / "libs" / "flux").glob("libflux.so*"))
            if not found:
                continue
            lib = found[0]
        markers = _binary_markers(lib)
        if markers is None:
            continue
        inspected += 1
        if ATTRIBUTION_MARKER in markers:
            errors.append(
                f"{build_dir.name}: attribution name machinery present without "
                f"-Dobserve_attribution (ADR-0129: debug-only must mean absent)"
            )
    if inspected == 0:
        print("note: leaks/attribution not verified — no readable library found",
              file=sys.stderr)


def check_leaks(repo_root: Path) -> list[str]:
    errors: list[str] = []
    kinds = collect_kinds(repo_root)

    lanes, lane_errors = parse_toml_ledger(repo_root / "tools" / "config" / "leak-lanes.toml")
    errors.extend(lane_errors)
    exempt, exempt_errors = parse_toml_ledger(repo_root / "tools" / "config" / "leak-exempt.toml")
    errors.extend(exempt_errors)

    tests = registered_tests(repo_root)

    # Every ledger table name is `kind.<symbol>` or `exempt.<symbol>`.
    lane_kinds = {k.split(".", 1)[1] for k in lanes if k.startswith("kind.")}
    exempt_kinds = {k.split(".", 1)[1] for k in exempt if k.startswith("exempt.")}
    covered = lane_kinds | exempt_kinds

    for kind in sorted(kinds):
        if kind not in covered:
            symbols = ", ".join(kinds[kind]["symbols"])
            errors.append(
                f"uncovered resource kind: {kind} ({symbols}) — add it to "
                f"tools/config/leak-lanes.toml with a lane, or to "
                f"leak-exempt.toml with a reason (INV-ARCH-LEDGER)"
            )

    # Stale ledger entries are their own defect: an exemption for a symbol
    # that no longer exists silently widens the exemption space.
    for kind in sorted(covered):
        if kind not in kinds:
            errors.append(f"stale ledger entry: {kind} is not a public kind in any header")

    # A kind with no release verb cannot have a lane; it must be exempted with
    # a reason instead. Putting one in the lane table would be a lie.
    for kind, info in sorted(kinds.items()):
        if info["owner"] is None and kind in lane_kinds:
            errors.append(
                f"{kind}: has no release verb but is registered with a lane — "
                f"it belongs in leak-exempt.toml with a reason"
            )

    # Lane hygiene: the file must exist and be registered as a test.
    for key, entry in sorted(lanes.items()):
        if not key.startswith("kind."):
            continue
        lane = entry.get("lane")
        if not lane:
            errors.append(f"{key}: missing `lane`")
            continue
        lane_path = repo_root / lane
        if not lane_path.is_file():
            errors.append(f"{key}: lane file does not exist: {lane}")
            continue
        if tests is not None:
            stem = lane_path.stem
            binary = stem[5:] if stem.startswith("test_") else stem
            if binary not in tests and stem not in tests:
                errors.append(
                    f"{key}: lane {lane} is not registered as a meson test "
                    f"(a test that never runs is not coverage)"
                )

    # Exemption hygiene: a reason is mandatory.
    for key, entry in sorted(exempt.items()):
        if not key.startswith("exempt."):
            continue
        reason = entry.get("reason", "").strip()
        if not reason:
            errors.append(f"{key}: exemption without a `reason` field")

    check_attribution(repo_root, errors)
    errors.extend(check_enforcement(repo_root))
    errors.extend(check_capability_consumers(repo_root))
    errors.extend(check_forbidden_calls(repo_root))
    errors.extend(check_raw_alloc(repo_root))
    return errors


# Ledger capabilities the ADR declares must actually be used. A scope, a
# lifetime or a capability that no call site exercises is a claim the tree does
# not back: the enforcement machinery exists, is tested, and can never fire.
# (This check exists because both cases had already happened here — the
# frame-scope drain was behind an undefined macro, and no kind was declared
# frame-lifetime at all, so the per-frame check had nothing to check.)
# Raw descriptor and site-string spellings that bypass the documented funnels.
# A leak detector with an escape hatch is a detector that will be escaped from:
# before this check, iris closed descriptors in six places outside the wrapper
# and a single `#include <unistd.h>` was enough to leave the ledger blind.
FORBIDDEN_CALLS = (
    (re.compile(r"(?<![A-Za-z0-9_])close\s*\(\s*(?!-1)"), "libs/iris",
     "raw close() in iris — use iris_fd_close_record so the row balances (ADR-0129)"),
    (re.compile(r"(?<![A-Za-z0-9_])dup\s*\("), "libs/iris",
     "raw dup() in iris — account the new descriptor with iris_fd_open_record"),
    (re.compile(r"__FILE__\s*\"\s*:\s*\""), "libs",
     "per-call-site __FILE__ site string - use a flux_site slot; one literal per "
     "call site is exactly the machinery a non-attribution build must not carry "
     "(ADR-0129)"),
)

# Every capability the ledger *declares* must have a consumer outside the
# ledger itself, or it is a switch that can never be flipped on. This list is
# derived from <flux/observe.h> at check time, not written here — a hardcoded
# whitelist is just a self-granted exemption, which is the shape
# (ADR-0130) forbids. Each capability is bound to the consumer that must
# mention it; a capability with no entry here fails outright, so adding a
# capability to the header forces a decision about who uses it.
CAPABILITY_CONSUMERS = {
    "FLUX_OBSERVE_COUNTS": None,  # unconditional; see check_capability_consumers
    "FLUX_OBSERVE_NAMES": ("libs", "flux_observe_name_object"),
    "FLUX_OBSERVE_SITES": ("libs", "FLUX_OBSERVE_SITES"),
    "FLUX_OBSERVE_PASS_LABELS": ("libs", "flux_vk_cmd_begin_label"),
}

# Drain points that exist as machinery and must be exercised by a real kind.
# Raw libc allocation in a library that owns a device is a contract violation
# (ADR-0135): it bypasses the consumer's allocator *and* the ledger. Only these
# are permitted, and only for the reasons stated.
#
#   txt_platform_font_*  allocate foreign objects (CFString storage, DWrite
#                        buffers, FcPattern nodes) that are freed by the foreign
#                        API inside its own allocation domain. Routing them
#                        through the caller's allocator would be incorrect.
#   app_win32 / app_cocoa
#                        OS handles are not allocations; the platform layer owns
#                        them through its own API.
RAW_ALLOC_EXEMPT = (
    "txt_platform_font_coretext.c",
    "txt_platform_font_directwrite.c",
    "txt_platform_font_fontconfig.c",
)

RAW_ALLOC_RE = re.compile(r"(?<![A-Za-z0-9_])(malloc|calloc|realloc|free)\s*\(")

# Device-less-by-design files (ADR-0136). Only files classified here may use a
# RAW_ALLOC_OK marker; a context-bound file cannot silence the check by adding
# one. Each entry states the property, not a convenience.
DEVICE_LESS_FILES = {
    "libs/lens/src/icon_svg.c": "SVG parser: text-only input, no context by design",
    "libs/lens/src/icon_runtime.c": "process-level icon registry: outlives every context",
    "libs/glyph/src/face.c": "glyph_ctx outlives its device association; platform memories",
    "libs/glyph/src/glyph_cache.c": "device-less cache fallback for measure-only contexts",
    "libs/lens/src/core/context.c": "the lens object itself predates its device association",
    "libs/lens/src/render/replay.c": "snapshot semantics strings are strdup'd, so libc owns them",
}

# The closed marker vocabulary. A marker outside this set is an error, so the
# vocabulary cannot quietly grow.
RAW_ALLOC_MARKERS = (
    "RAW_ALLOC_OK(process-registry)",
    "RAW_ALLOC_OK(no-device)",
    "RAW_ALLOC_OK(paired-platform)",
    "RAW_ALLOC_OK(transient)",
)

REQUIRED_ENFORCEMENT = (
    (
        "FLUX_LIFETIME_FRAME",
        "libs",
        "no kind is declared FLUX_LIFETIME_FRAME, so "
        "flux_observe_frame_drain_check has nothing to enforce "
        "(ADR-0129 decision 4 is declared but unused)",
    ),
    (
        "flux_observe_report_emit_process",
        "libs/iris",
        "the process ledger is never drained by the backend that owns the "
        "process-scoped descriptors (ADR-0129)",
    ),
    (
        "flux_observe_name_object",
        "libs",
        "no object is named from the ledger, so FLUX_OBSERVE_NAMES produces "
        "names nobody can see (ADR-0132)",
    ),
    (
        "flux_vk_cmd_begin_label",
        "libs",
        "no pass is labelled, so FLUX_OBSERVE_PASS_LABELS annotates nothing "
        "(ADR-0133)",
    ),
)


def declared_capabilities(repo_root: Path) -> list[str]:
    header = repo_root / "libs" / "flux" / "include" / "flux" / "observe.h"
    if not header.is_file():
        return []
    return re.findall(r"(FLUX_OBSERVE_[A-Z_]+)\s*=\s*1u\s*<<", header.read_text(encoding="utf-8"))


def _mentions(repo_root: Path, scope: str, needle: str) -> bool:
    roots = [repo_root / scope]
    if scope == "libs":
        roots = sorted((repo_root / "libs").iterdir())
    for root in roots:
        if not root.is_dir():
            continue
        for path in root.rglob("*"):
            if path.suffix not in (".c", ".h"):
                continue
            # The ledger's own implementation always mentions its capabilities;
            # a consumer must exist elsewhere.
            if path.name in ("observe.h", "observe.c"):
                continue
            try:
                if needle in path.read_text(encoding="utf-8"):
                    return True
            except Exception:  # noqa: BLE001
                continue
    return False


def check_capability_consumers(repo_root: Path) -> list[str]:
    """Every declared capability has a consumer, or must be removed.

    A capability bit that nothing reads is worse than a missing feature: it
    documents a diagnostic that will never appear. FLUX_OBSERVE_NAMES and
    FLUX_OBSERVE_PASS_LABELS both shipped in that state.
    """
    errors = []
    for cap in declared_capabilities(repo_root):
        if cap == "FLUX_OBSERVE_COUNTS":
            # Not opt-in: it is the always-on baseline. It is exercised by the
            # very existence of a ledger row, so the consumer is a counted
            # acquire anywhere outside the ledger.
            ok = _mentions(repo_root, "libs", "FLUX_OBSERVE_ACQUIRE(")
            if not ok:
                errors.append(f"{cap}: no counted acquire anywhere — the baseline is unused")
            continue
        consumer = CAPABILITY_CONSUMERS.get(cap)
        if consumer is None:
            errors.append(
                f"{cap}: declared in <flux/observe.h> but no consumer is registered "
                f"for it; either implement it and add the binding in "
                f"tools/guardrails/leaks.py, or delete the capability (ADR-0130)"
            )
            continue
        scope, needle = consumer
        if not _mentions(repo_root, scope, needle):
            errors.append(
                f"{cap}: declared but nothing outside the ledger reads it — a switch "
                f"that can never be turned on documents a diagnostic that never appears"
            )
    return errors


def check_raw_alloc(repo_root: Path) -> list[str]:
    """Libraries with a device must allocate through flux_device_alloc."""
    errors = []
    for lib in ("prism", "lens", "glyph"):
        root = repo_root / "libs" / lib / "src"
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*.c")):
            if path.name in RAW_ALLOC_EXEMPT:
                continue
            try:
                raw_text = path.read_text(encoding="utf-8")
            except Exception:  # noqa: BLE001
                continue
            # Markers live in comments, so they must be read from the raw text;
            # code is matched against the comment-stripped text so a documented
            # `free(` is not mistaken for a call.
            stripped = strip_c_comments(raw_text).splitlines()
            raw_lines = raw_text.splitlines()
            rel = path.relative_to(repo_root).as_posix()
            device_less = rel in DEVICE_LESS_FILES
            for lineno, line in enumerate(stripped, start=1):
                raw_line = raw_lines[lineno - 1] if lineno <= len(raw_lines) else ""
                marker = next((mk for mk in RAW_ALLOC_MARKERS if mk in raw_line), None)
                if marker:
                    if not device_less:
                        errors.append(
                            f"{rel}:{lineno}: RAW_ALLOC_OK in a context-bound file — only "
                            f"files classified in DEVICE_LESS_FILES may suppress this "
                            f"check (ADR-0136)"
                        )
                    continue
                if "RAW_ALLOC_OK" in raw_line:
                    errors.append(
                        f"{rel}:{lineno}: RAW_ALLOC_OK marker outside the closed vocabulary "
                        f"(one of {', '.join(RAW_ALLOC_MARKERS)}) — the exceptions are "
                        f"architectural properties, not a free-form excuse (ADR-0136)"
                    )
                    continue
                m = RAW_ALLOC_RE.search(line)
                if not m:
                    continue
                errors.append(
                    f"{rel}:{lineno}: raw {m.group(1)}() in {lib} — allocate through "
                    f"flux_device_alloc so the consumer's allocator is honoured and the "
                    f"memory is attributed (ADR-0135). A device-less path needs a marker "
                    f"from the closed vocabulary and a DEVICE_LESS_FILES entry (ADR-0136)."
                )
    return errors


def check_forbidden_calls(repo_root: Path) -> list[str]:
    """Raw spellings that bypass the documented accounting funnels."""
    errors = []
    for pattern, scope, message in FORBIDDEN_CALLS:
        root = repo_root / scope
        if not root.is_dir():
            continue
        for path in sorted(root.rglob("*")):
            if path.suffix not in (".c", ".h"):
                continue
            # The wrappers themselves are the funnel; they must use the raw call.
            if path.name in ("iris_fd_observe.h", "observe.h", "observe_sites.h"):
                continue
            try:
                text = strip_c_comments(path.read_text(encoding="utf-8"))
            except Exception:  # noqa: BLE001
                continue
            for lineno, line in enumerate(text.splitlines(), start=1):
                if pattern.search(line):
                    rel = path.relative_to(repo_root).as_posix()
                    errors.append(f"{rel}:{lineno}: {message}")
    return errors


def check_enforcement(repo_root: Path) -> list[str]:
    """Declared-but-unused ledger capabilities.

    Only meaningful in a tree that actually contains the ledger's consumers, so
    it is skipped when the library sources are absent (the guardrail's own
    fixture tests build a minimal tree with one header and no libs/*/src).
    """
    errors = []
    if not (repo_root / "libs" / "flux" / "src").is_dir():
        return errors
    for needle, scope, message in REQUIRED_ENFORCEMENT:
        roots = [repo_root / scope]
        if scope == "libs":
            roots = sorted((repo_root / "libs").iterdir())
        hits = []
        for root in roots:
            if not root.is_dir():
                continue
            for path in root.rglob("*"):
                if path.suffix not in (".c", ".h"):
                    continue
                if path.name == "observe.h" or path.name == "observe.c":
                    continue
                try:
                    if needle in path.read_text(encoding="utf-8"):
                        hits.append(path)
                except Exception:  # noqa: BLE001
                    continue
        if not hits:
            errors.append(message)
    return errors


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent.parent
    errors = check_leaks(repo_root)
    if errors:
        sys.stderr.write(
            "error: leak coverage is incomplete (ADR-0130, INV-ARCH-LEDGER):\n")
        for e in errors:
            sys.stderr.write(f"  {e}\n")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
