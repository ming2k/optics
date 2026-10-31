#!/usr/bin/env python3
"""Native documentation hyperlink, audience firewall, and architectural lineage verification.

Enforces:
  1. Target file existence (style.md §1.1)
  2. No absolute locators (style.md §1.1)
  3. Heading anchor resolution (style.md §1.2)
  4. Audience firewall (taxonomy.md §2: public docs must not link to docs/dev/)
  5. Root Markdown whitelist (taxonomy.md §3)
  6. ADR reference lineage (adr.md §2: ADR-NNNN must resolve to docs/adr/)
"""

import os
import re
import sys

EXTERNAL_PREFIXES = ("http://", "https://", "mailto:", "tel:", "data:", "javascript:")

PUBLIC_SURFACES = (
    "docs/tutorials/",
    "docs/how-to/",
    "docs/reference/",
    "docs/explanation/",
)

INTERNAL_SPACE = "docs/dev/"

ROOT_WHITELIST = {
    "README.md",
    "CHANGELOG.md",
    "CONTRIBUTING.md",
    "AGENTS.md",
    "LICENSE.md",
    "SECURITY.md",
}


def github_slug(text: str) -> str:
    """Generate GitHub-compatible markdown heading slug."""
    lower = text.strip().lower()
    no_code = re.sub(r"`([^`]*)`", r"\1", lower)
    no_link = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", no_code)
    no_html = re.sub(r"<[^>]+>", "", no_link)
    clean = re.sub(r"[^\w\s\-]", "", no_html)
    return re.sub(r"\s+", "-", clean.strip())


def extract_heading_anchors(path: str) -> set:
    """Extract all heading anchors from a markdown file."""
    anchors = set()
    seen = {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            content = f.read()
    except Exception:
        return anchors

    in_fence = False
    for line in content.splitlines():
        trimmed = line.strip()
        if trimmed.startswith("```") or trimmed.startswith("~~~"):
            in_fence = not in_fence
            continue
        if in_fence:
            continue

        match = re.match(r"^(#{1,6})\s+(.*?)\s*#*$", line)
        if match:
            heading_text = match.group(2)
            base = github_slug(heading_text)
            count = seen.get(base, 0)
            seen[base] = count + 1
            anchor = base if count == 0 else f"{base}-{count}"
            anchors.add(anchor)

    return anchors


def get_canonical_adrs(root_dir: str) -> set:
    """Get all existing ADR IDs (e.g. '0001', '0125')."""
    adrs = set()
    adr_dir = os.path.join(root_dir, "docs", "adr")
    if not os.path.isdir(adr_dir):
        return adrs

    for name in os.listdir(adr_dir):
        m = re.match(r"^(\d{4})-.*\.md$", name)
        if m:
            adrs.add(m.group(1))
    return adrs


def find_markdown_files(root_dir: str):
    """Find all relevant markdown files to verify."""
    md_files = []
    ignore_dirs = {".git", "build", "build-release", "build-tidy", "subprojects", "target"}

    for root, dirs, files in os.walk(root_dir):
        dirs[:] = [d for d in dirs if d not in ignore_dirs and not d.startswith(".")]
        for f in files:
            if f.endswith(".md"):
                full_path = os.path.join(root, f)
                rel_path = os.path.relpath(full_path, root_dir).replace("\\", "/")
                # Check root files or files under docs/, examples/, tools/, libs/
                if "/" not in rel_path or rel_path.startswith("docs/") or rel_path.startswith("examples/") or rel_path.startswith("tools/"):
                    md_files.append((full_path, rel_path))

    md_files.sort(key=lambda x: x[1])
    return md_files


def check_documentation(root_dir: str) -> list[str]:
    problems = []
    canonical_adrs = get_canonical_adrs(root_dir)
    md_files = find_markdown_files(root_dir)
    anchor_cache = {}

    link_re = re.compile(r"!?\[[^\]]*\]\(\s*<?([^)\s>]+)>?\s*\)")
    adr_mention_re = re.compile(r"\bADR-(\d{4})\b")

    # 1. Root Markdown Whitelist Check
    for full_path, rel_path in md_files:
        if "/" not in rel_path:
            if rel_path not in ROOT_WHITELIST:
                problems.append(
                    f"{rel_path}: root markdown file violates whitelist policy (allowed: {sorted(ROOT_WHITELIST)})"
                )

    # 2. Hyperlink & Firewall Verification
    for full_path, rel_path in md_files:
        try:
            with open(full_path, "r", encoding="utf-8") as f:
                lines = f.readlines()
        except Exception as e:
            problems.append(f"{rel_path}: cannot read file: {e}")
            continue

        parent_dir = os.path.dirname(full_path)
        in_fence = False

        for idx, line in enumerate(lines, start=1):
            trimmed = line.strip()
            if trimmed.startswith("```") or trimmed.startswith("~~~"):
                in_fence = not in_fence
                continue
            if in_fence:
                continue

            # Strip inline code spans to avoid inspecting syntax examples
            line_prose = re.sub(r"`[^`]*`", "", line)

            # Check ADR-NNNN citations
            for adr_match in adr_mention_re.finditer(line_prose):
                adr_num = adr_match.group(1)
                if adr_num not in canonical_adrs:
                    # Allow external/legacy references if explicitly qualified in nearby context
                    ctx_start = max(0, idx - 3)
                    ctx_end = min(len(lines), idx + 2)
                    context_str = " ".join(lines[ctx_start:ctx_end]).lower()
                    if "aegis" in context_str or "legacy" in context_str or "superseded" in context_str or "external" in context_str:
                        continue
                    problems.append(
                        f"{rel_path}:{idx}: references ADR-{adr_num} which does not exist in docs/adr/"
                    )

            # Check markdown links
            for match in link_re.finditer(line_prose):
                target = match.group(1).strip()
                if not target or target.startswith(EXTERNAL_PREFIXES):
                    continue

                location = f"{rel_path}:{idx}"

                if target.startswith("/") or target.startswith("file://"):
                    problems.append(f"{location}: absolute locator `{target}` (use a relative path)")
                    continue

                if target.startswith("#"):
                    frag = target[1:].lower()
                    if full_path not in anchor_cache:
                        anchor_cache[full_path] = extract_heading_anchors(full_path)
                    if frag not in anchor_cache[full_path]:
                        problems.append(f"{location}: same-file anchor `#{frag}` does not exist")
                    continue

                file_part, _, fragment = target.partition("#")
                if not file_part:
                    continue

                target_path = os.path.normpath(os.path.join(parent_dir, file_part))
                if not os.path.exists(target_path):
                    problems.append(f"{location}: target `{target}` does not resolve to an existing file")
                    continue

                # Audience Firewall: public surfaces must never link into docs/dev/
                if any(rel_path.startswith(surface) for surface in PUBLIC_SURFACES):
                    target_rel = os.path.relpath(target_path, root_dir).replace("\\", "/")
                    if target_rel.startswith(INTERNAL_SPACE):
                        problems.append(
                            f"{location}: `{target}` crosses the audience firewall (public docs must not link into {INTERNAL_SPACE})"
                        )

                # Heading anchor verification in target file
                if fragment:
                    frag_lower = fragment.lower()
                    if target_path not in anchor_cache:
                        anchor_cache[target_path] = extract_heading_anchors(target_path)
                    if target_path.endswith(".md") and anchor_cache[target_path]:
                        if frag_lower not in anchor_cache[target_path]:
                            problems.append(
                                f"{location}: anchor `#{fragment}` does not exist in `{file_part}`"
                            )

    return problems


def main() -> int:
    root_dir = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
    problems = check_documentation(root_dir)

    if problems:
        print(f"Documentation verification failed with {len(problems)} violation(s):", file=sys.stderr)
        for p in problems:
            print(f"  ERROR: {p}", file=sys.stderr)
        return 1

    print("✔ Documentation integrity, hyperlinks, audience firewall, and ADR lineage verified.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
