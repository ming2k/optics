# Developer Documentation

These pages cover the internal engineering, testing, maintenance, and quality standards for the Optics monorepo:

- **[Engineering Standards](standards/README.md)** — Repository architecture topology, system invariants, C23 API guidelines, UI component purity, and PR review gates.
- **[Setup](setup.md)** — Install dependencies, configure the build matrix, and manage toolchains.
- **[Testing](testing.md)** — Run, select, debug, and extend the C, Rust, and benchmark test suites.
- **[Manual Testing Runbooks](manual-testing.md)** — Interactive verification, graphics inspection, UI/input checks, and platform matrix runbooks.
- **[Cross-Platform Invariants](cross-platform.md)** — Linux (Wayland), Windows (Win32), and macOS (Cocoa/MoltenVK) requirements and verification tooling.
- **[Project Layout](project-layout.md)** — Structural map of libraries, tests, examples, bindings, and documentation assets.
- **[Release & Versioning](release.md)** — Release checklist, version lockstep, and packaging procedures.
- **[Animation, Opacity & Layer Performance](animation-and-layer-performance.md)** — Premultiplied command opacity invariants, scissor-bounded layer passes, stencil synchronization, and GPU glyph batching ([ADR-0109](../adr/0109-premultiplied-command-opacity-scissor-bounded-layers-and-glyph-run-batching.md)).
- **[Architecture Implementation Review](architecture-review-2026-09-14.md)** — Architectural inspection, work packages, and verification evidence for ADR-0089–0094.

User-facing rendering tasks belong in the [Tutorials](../tutorials/index.md) and [How-To Guides](../how-to/index.md).
