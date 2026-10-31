# Documentation & Markdown Style Guidelines

This document codifies the hyperlink, formatting, and prose conventions across all documentation in Optics.

---

## 1. Canonical Hyperlink Conventions

1. **Relative Path Routing**:
   - All internal links between documentation files must use explicit relative paths (e.g. `../reference/api.md`, `../adr/0001-project-foundations.md`).
   - Never use absolute file system paths (e.g. `/docs/...`) or remote GitHub URLs (`https://github.com/.../blob/main/docs/...`) for files within the repository.
2. **Anchor Resolution**:
   - Heading anchors must match the lowercase, hyphen-separated GitHub markdown slugification of the target heading.
   - Anchor links must point to existing headings in the target document.
3. **Explicit Markdown Extensions**:
   - Always append `.md` to file paths in markdown links (e.g. `[Tutorial](01-getting-started.md)` rather than extensionless paths).

---

## 2. Formatting & Prose Guidelines

1. **Header Hierarchy**:
   - Each markdown document must contain exactly one top-level `# Title` heading.
   - Subsections must progress logically (`##`, `###`, `####`) without skipping levels.
2. **Code Blocks & Fences**:
   - Always declare explicit language specifiers on fenced code blocks (e.g. ````c````, ````rust````, ````bash````, ````text````).
   - Use standard C23 constructs in C code examples: `nullptr`, `{}` zero-init, `const module_item_opts *opts`.
3. **Structured Tables**:
   - Tables must contain a clean header row and alignment delimiters (`:---`, `:---:`).
4. **Active, Clear Voice**:
   - Prefer concise, imperative, technically precise prose.
   - Avoid emotional language, colloquialisms, or filler statements.
