# Engineering Workflow & Verification Gates

This document codifies the operational workflows, PR review gates, and verification criteria for changes to Optics.

---

## 1. Pull Request Review Gates

Before any pull request is merged into `main`, it must clear the following verification gates:

```text
┌─────────────────────────────────────────────────────────────┐
│ 1. Unified Guardrails Gate: `tools/optics check`            │
│    - Format check (clang-format 22.x)                       │
│    - Material boundary separation (ADR-0063)                │
│    - Version lockstep across sibling libraries              │
│    - Library stdout hygiene (stderr only)                   │
│    - Public symbols reference freshness (`symbols.py`)      │
│    - Documentation integrity & audience firewall            │
├─────────────────────────────────────────────────────────────┤
│ 2. Automated Test Gate: `meson test -C build`               │
│    All C unit, integration, and benchmark tests pass        │
├─────────────────────────────────────────────────────────────┤
│ 3. Sanitizer Gate: ASan + UBSan Cleanliness                 │
│    Zero leaks, zero undefined behavior, zero alloc faults   │
├─────────────────────────────────────────────────────────────┤
│ 4. Rust Bindings Gate: `cargo test --workspace`             │
│    All safe bindings in `bindings/` compile and pass tests  │
├─────────────────────────────────────────────────────────────┤
│ 5. Code-to-Doc Synchronization Gate                         │
│    Public API or architecture changes matched with docs     │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. Code-to-Doc Synchronization (Atomic Commits)

Any pull request that introduces or modifies developer-visible or integrator-facing surfaces must update the corresponding documentation within the **same atomic commit / pull request**:

| Modified Surface | Required Documentation Synchronization |
| :--- | :--- |
| Public C API headers (`libs/*/include/**/*.h`) | `docs/reference/api.md` & symbol index (`./tools/optics check symbols`) |
| Major architectural restructuring or clean breaks | New or amended ADR in `docs/adr/` |
| Developer-facing or integrator-facing guides | Corresponding task recipes in `docs/how-to/` or `docs/tutorials/` |
| Conceptual systems or engine foundations | Corresponding deep dives in `docs/explanation/` or `docs/reference/` |

---

## 3. Pre-Release Quality Checklist

Prior to creating a release tag:

1. **Run Unified Guardrails**: Execute `./tools/optics check` and ensure all checks report `OK`.
2. **Full Test Matrix**: Run `meson test -C build` and verify 100% pass rate.
3. **Rust Bindings Verification**: Run `cargo test` inside `bindings/` to verify safe bindings.
4. **Symbol Index Freshness**: Ensure `./tools/optics check symbols` passes without drift.
5. **Changelog Integrity**: Ensure user-facing features, breaking changes, and migrations are recorded in `CHANGELOG.md`.
6. **Clean Git State**: Verify no untracked experimental files or temporary debug assets remain in the tree (`git status --porcelain` is clean).
