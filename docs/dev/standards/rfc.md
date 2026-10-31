# Request for Comments (RFC) Standards

This document establishes the deliberation, review, and consensus protocol for cross-cutting subsystem redesigns in Optics prior to ADR ratification.

---

## 1. RFC vs. ADR: Clear Separation of Concerns

- **RFC (Request for Comments)**: An exploratory, deliberative proposal used to debate complex problem spaces, evaluate competing technical tradeoffs, and gather consensus across multiple library boundaries (e.g. redesigning the 2D vector curve kernel or introducing hardware subsurfaces).
- **ADR (Architectural Decision Record)**: The binding, permanent architectural ledger that records the final ratified contract, active invariants, and discarded alternatives.

An RFC precedes an ADR when the solution space is uncertain or requires cross-domain collaboration. Once consensus is reached, the conclusion is codified as one or more canonical ADRs.

---

## 2. RFC Lifecycle & Gates

```text
[ Proposal Draft ] ──▶ [ Active Deliberation ] ──▶ [ Consensus / Ratification ]
                                                        │
                                                        ├──▶ [ Ratified ➔ ADR Landed ]
                                                        └──▶ [ Closed / Withdrawn ]
```

1. **Intake**: Author creates the proposal document detailing the problem statement, proposed API/pipeline changes, and benchmark evidence.
2. **Review & Prototyping**: Engineering evaluates memory consumption, frame pacing, GPU compute shader bandwidth, and C ABI impact.
3. **Ratification**: The architectural working group approves the design; canonical ADRs are authored and added to `docs/adr/`.
4. **Excise & Land**: Once implementation merges and lands, obsolete code is removed according to the Zero-Baggage Deprecation Policy.
