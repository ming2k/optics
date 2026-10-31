# Blameless Postmortem Standards

This document establishes the standards, principles, and structure for post-incident analyses and retrospective investigations in Optics.

---

## 1. Core Principles

1. **Strictly Blameless**:
   - Systems fail; individuals do not. Postmortems analyze systemic defenses, tooling gaps, and detection latencies rather than assigning personal blame.
2. **Timeline Fidelity**:
   - Establish an objective chronological log of events from introduction to detection, triage, and mitigation.
3. **Defense-in-Depth & Mechanical Remediation**:
   - Every remediation action item must be a mechanical gate (e.g. an automated CI check, compiler static assertion, or architecture firewall test), never a verbal instruction or behavioral pledge.

---

## 2. Canonical Postmortem Structure

When an incident or regression occurs (e.g. GPU memory leak, presentation lockup, ABI break), author a document under `docs/dev/postmortems/YYYY-MM-DD-<slug>.md` following this structure:

```markdown
# Incident Postmortem: YYYY-MM-DD [Incident Title]

- Date: YYYY-MM-DD
- Severity: Critical | High | Medium
- Affected Modules: [e.g. libs/flux, libs/iris]
- Authors: Optics Engineering Team

---

## 1. Executive Summary
Brief high-level overview of what occurred, user-visible impact, and final resolution.

## 2. Chronological Timeline
- `YYYY-MM-DD HH:MM UTC`: Change introduced in commit `abc1234`.
- `YYYY-MM-DD HH:MM UTC`: Anomaly detected via automated nightly CI run.
- `YYYY-MM-DD HH:MM UTC`: Root cause identified.
- `YYYY-MM-DD HH:MM UTC`: Fix verified and merged.

## 3. Root Cause Analysis (5 Whys)
Detailed technical diagnosis of the fundamental trigger and state corruption mechanism.

## 4. Detection Gaps
Why did existing unit tests, sanitizers, or integration tests fail to detect the failure prior to merge?

## 5. Preventative Action Items
| Action Item | Owner | Target Gate | Verification |
| :--- | :--- | :--- | :--- |
| Add new ASan leak regression test | Eng | `meson test` | Automated in CI |
| Codify new architectural red-line | Arch | `docs/dev/standards/` | Guardrail script |
```
