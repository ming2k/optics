# Engineering Workflow & Verification Gates

This document codifies the operational workflows, PR review gates, and verification criteria for changes to Optics.

---

## 1. Pull Request Review Gates

Before any pull request is merged into `main`, it must clear the following verification gates:

```text
┌─────────────────────────────────────────────────────────────┐
│ 1. Formatting Gate: `./tools/format.sh --check`             │
│    All C sources conform to clang-format                    │
├─────────────────────────────────────────────────────────────┤
│ 2. Test Gate: unit → integration → e2e + lint/tooling       │
│    Every layer passes; e2e is public-API only               │
├─────────────────────────────────────────────────────────────┤
│ 3. Sanitizer Gate: ASan + UBSan Cleanliness                 │
│    Zero leaks, zero undefined behavior, zero alloc faults   │
├─────────────────────────────────────────────────────────────┤
│ 4. Rust Bindings Gate: `cargo test --workspace`             │
│    All crates under `bindings/crates/` compile and pass     │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. Code-to-Doc Synchronization (Atomic Commits)

Any pull request that introduces or modifies developer-visible or integrator-facing surfaces must update the corresponding documentation within the **same atomic commit / pull request**:

| Modified Surface | Required Documentation Synchronization |
| :--- | :--- |
| Public C API headers (`libs/*/include/**/*.h`) | `docs/reference/api.md` |
| Major architectural restructuring or clean breaks | New or amended ADR in `docs/adr/` |
| Developer-facing or integrator-facing guides | Corresponding task recipes in `docs/how-to/` or `docs/tutorials/` |
| Conceptual systems or engine foundations | Corresponding deep dives in `docs/explanation/` or `docs/reference/` |

---

## 3. Pre-Release Quality Checklist

Prior to creating a release tag:

1. **Formatting**: Run `./tools/format.sh --check` to ensure clean code style.
2. **Layer Test Matrix**: Run each layer explicitly and verify 100% pass/skip on its own gate:
   `meson test -C build --suite unit`, `--suite integration`, and `--suite e2e`.
   The `test_layers` lint gate must also pass; it rejects unclassified tests and private harnesses.
3. **Rust Bindings Verification**: Run `cargo test --workspace` from the repository root to verify safe bindings.
4. **Changelog Integrity**: Ensure user-facing features, breaking changes, and migrations are recorded in `CHANGELOG.md`.
5. **Clean Git State**: Verify no untracked experimental files or temporary debug assets remain in the tree (`git status --porcelain` is clean).
