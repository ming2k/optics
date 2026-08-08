# ADR-0054: Font discovery as a platform layer (fontconfig / DirectWrite / CoreText)

- Status: Accepted
- Date: 2026-08-08
- Scope: flux-text (`libs/flux/text`). Font *discovery and fallback* only —
  rasterisation stays FreeType, shaping stays HarfBuzz, BiDi stays FriBidi
  on every platform.

## Context

flux-text resolved fonts exclusively through fontconfig
(`FcFontSort`/`FcPatternBuild`/`FcCharSetHasChar` in `face.c`), plus a
hard-coded `/usr/share/fonts` last-resort path. Fontconfig is not native
on Windows or macOS: shipping it there means a second, OS-invisible font
configuration whose fallback choices diverge from every native
application. What must be identical across platforms is the *semantics*
of the current code: a ranked candidate chain for (family, weight,
italic), and lazy per-codepoint "patch faces" for CJK/emoji/historic
scripts.

## Decision

1. **Internal seam `src/txt_platform_font.h`.** One platform-neutral
   interface carries the full fontconfig semantics:
   `txt_platform_font_query_family` (ranked `txtp_font_list`, best
   first), `txt_platform_font_query_codepoint` (per-codepoint fallback),
   an opaque `txtp_charset` coverage set, and a FreeType-compatible face
   index (TTC member; on fontconfig also the variable-font named
   instance in the high bits). Family names are UTF-8; generic names
   (`sans-serif`/`serif`/`monospace`) map to platform defaults;
   `weight` is CSS 1..1000.
2. **Three backends, exactly one compiled per host** (selected in
   `libs/flux/text/meson.build`):
   - `txt_platform_font_fontconfig.c` — Linux; the original code moved
     verbatim, byte-identical behaviour, `/usr/share/fonts` fallback
     stays here.
   - `txt_platform_font_directwrite.c` — Windows; pure-C COM
     (`IDWriteFactory`, `GetMatchingFonts`,
     `IDWriteFontFallback::MapCharacters` for per-codepoint, local font
     file loader for paths). Compile-verified against real MinGW-w64
     headers via zig cc in CI.
   - `txt_platform_font_coretext.c` — macOS; pure-C CoreText
     (`CTFontDescriptorCreateMatchingFontDescriptors`,
     `CTFontCreateForCharacters`, `kCTFontURLAttribute` for paths).
3. `face.c` calls only the seam; `FcChar32` disappears from shared code.

## Alternatives Considered

- **fontconfig on all platforms.** Rejected: invisible to OS font
  settings, needs a shipped font cache configuration, and produces
  fallback choices inconsistent with native apps — the opposite of the
  consistency goal.
- **DirectWrite/CoreText for rasterisation too.** Rejected: FreeType
  rasterisation is the cross-platform constant; keeping one rasteriser
  avoids three-way glyph-metric divergence.
- **A "find font file by name" minimal interface.** Rejected: it would
  silently drop the ranked-chain and patch-face semantics that Linux
  tests exercise; the seam must carry everything fontconfig did.

## Consequences

- Per-platform fallback *choices* may legitimately differ (the OS owns
  them); what stays constant is the pipeline shape and the generic-name
  contract. Documented in `docs/dev/cross-platform.md`.
- CoreText cannot report a FreeType face index for `.ttc` members
  (returns 0; traits still guarantee the right member in practice —
  noted in the file header for a future PostScript-name refinement).
- Weight-scale mapping (CSS → DirectWrite/CoreText) is an approximation
  at the extremes (100/900).
- DirectWrite's main chain does not span families the way fontconfig
  does; per-codepoint patch faces cover CJK/emoji instead.

## References

- `libs/flux/text/src/txt_platform_font.h` and the three backends.
- [ADR-0016](0016-pure-rhi-and-draw-primitives.md) — text as a sibling
  library; [ADR-0033](0033-lens-text-seam.md) — the lens text seam.
