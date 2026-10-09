---
id: ADR-0003
title: Admit platform tools read-only first and journal every mutation
status: proposed
date: 2026-10-09
deciders: [lumberbarons]
supersedes: []
superseded-by: []
related: [ADR-0001]
tags: [tools, safety, journal]
---

# ADR-0003: Admit platform tools read-only first and journal every mutation

## Context and Problem Statement

Classic Mac OS offers many subsystems a model-facing tool could expose: the
Resource Manager, Finder catalog metadata, the Process Manager and Apple Events,
QuickDraw metrics, OSA, sound and speech, the Trash. Seven read-only inspection
tools already exist (`get_file_info`, `resolve_alias`, `list_processes`,
`list_fonts`, `measure_text`, `list_resources`, `read_resource`). More are
wanted, some of which mutate the disk or other processes. Without a stated
admission rule each new tool re-argues what safety it owes, and the mutation
guarantees the existing file tools provide are easy to erode one tool at a time.

## Decision Drivers

- The file tools offer create-only writes, revision-guarded edits, an
  intent/staged/committed journal, verified staging, retained backups and
  explicit uncertain outcomes. A new mutating tool that skips any of this
  creates an unrecorded path to data loss.
- The guest has no memory protection or undo; the user is at a real machine.
- A tool result is recorded into history with `AGENT_RESULT_CAP` (1536 bytes), and
  the model pages through larger data with continuations. A tool whose output
  does not fit cannot be used reliably.
- Built artifacts are persisted evidence authorizing `run_application`. Editing
  or re-flagging one would invalidate that authority.
- Per the verification rule, nothing is advertised to the model before a real
  OS 9.2.2 run, and some subsystems (Speech, Translation Manager, OSA) are
  optional extensions whose presence on the guest is unconfirmed.

## Considered Options

1. **Tiered admission rule** — read-only tools first, mutations only through the
   existing journal model, artifacts immutable, results bounded.
2. **Case-by-case review** — decide safety per tool when it is proposed.
3. **Freeze the tool set** — add no further platform tools.

## Decision Outcome

Chosen option: **Tiered admission rule**.

Read-only tools are cheap to add safely and need no journal, so they go first and
strengthen the build, verify, run loop. A mutating tool must join the same
intent/staged/committed journal, staging, verification and uncertain-outcome
model; there are no silent retries and no raw delete primitive. Option 2 drifts;
option 3 forgoes the platform-specific tools (resources, Finder identity,
process lifecycle) that let the agent check what it actually produced.

## Consequences

### Positive

- New read-only tools have a predictable bar; reviewers can check a short list.
- No tool becomes a back door around the file tools' recovery guarantees.
- Failed fixtures and superseded outputs can be removed reversibly without a
  raw delete tool (Trash, not `rm`).

### Negative

- Mutating tools cost more: journal events, verification, host fault-injection
  tests and a guest diagnostic each.
- Every result must be paginated to fit the cap, which constrains tools with
  naturally large output such as resource maps and font lists.

### Risks

- Pressure to bypass the journal for "small" metadata changes. Watch for a tool
  PR that writes catalog or fork data without journal events.
- A generic script or shell tool would break the harness boundary (no generic
  shell or desktop control). Any such proposal needs its own ADR.

## Pros and Cons of the Options

### Tiered admission rule

- Good: explicit, checkable, preserves existing guarantees.
- Bad: slows mutating tools.

### Case-by-case review

- Good: flexible.
- Bad: inconsistent; weakens guarantees gradually.

### Freeze the tool set

- Good: no new risk.
- Bad: the agent stays blind to resources, Finder identity and processes.

## Implementation Notes

- Rules a new tool must satisfy:
  - Read-only tools need no journal. Results fit `AGENT_RESULT_CAP` and page with
    a `next_*` continuation where needed.
  - A mutating tool journals intent, staged and committed, verifies, and never
    retries silently; an uncertain outcome stops the run.
  - Catalog-metadata mutations (type/creator, label, lock, flags) never touch
    fork bytes. Whole-fork changes need whole-fork staging, verification and
    revisions.
  - Built artifacts stay immutable evidence. Resource or metadata edits target
    workspace sources, never a recorded build output; the agent edits sources
    and rebuilds.
  - Process-targeting Apple events are scoped to PSNs the harness launched,
    never the Finder or arbitrary user applications.
  - Optional-extension tools need a Gestalt preflight and a skip path; do not
    advertise one until a real guest run.
- Resource reading is a deliberate read-only exception to the file tools'
  resource-fork refusal. It does not extend to resource writes.
- Revisit if the result cap changes (see the limits guide) or if the product
  gains multimodal results, which change what "bounded" means.

## References

- ADR-0001 (cooperative Apple-event machinery shared by process tools)
- The tools guide for current contracts
