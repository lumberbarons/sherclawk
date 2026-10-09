---
id: ADR-0002
title: Run generated-project tests in a separate native application
status: proposed
date: 2026-10-09
deciders: [lumberbarons]
supersedes: []
superseded-by: []
related: [ADR-0001]
tags: [testing, build, execution]
---

# ADR-0002: Run generated-project tests in a separate native application

## Context and Problem Statement

`run_application` launches a built artifact and reports `process_present`, with
`smoke_test: not_performed`. Nothing tells the agent whether the code it wrote
works, so the edit, build, run loop cannot close on correctness. The proposed
`test_project(path)` tool needs somewhere to execute project-authored test
functions. They are untrusted, arbitrary C that may dereference bad pointers,
abort, or loop forever, on a platform with no memory protection and
cooperative scheduling.

## Decision Drivers

- Sherclawk's process holds the chat, journal and session state. A crash or hang
  there loses the harness itself, not just one test.
- OS 9 has no preemption or memory protection: a non-yielding application can
  block the whole machine. Isolation is partial at best, so exposure should
  be minimized, not assumed away.
- Sherclawk can already build, launch and observe a separate native app
  (ADR-0001, `run_application`), with persisted artifact authority.
- A Finder launch returns no Unix exit status, and process disappearance cannot
  distinguish a crash from a normal exit. Success must not be inferred from
  either.
- Results must be correlated to one build and one run so stale output can never
  satisfy a new request.
- Descriptors accept data only, never shell or MPW fragments.

## Considered Options

1. **Separate native test application** — project tests are compiled into their
   own app, launched by Sherclawk, which reads a correlated completion report.
2. **In-process execution** — link test code into Sherclawk or load it as a
   code resource.
3. **Host-side test command** — run tests through a shell on the host or in
   MacRelix.

## Decision Outcome

Chosen option: **Separate native test application**.

Option 2 lets a bad pointer, abort or infinite loop take down the harness and
its session. Option 3 reintroduces the MacRelix and host dependencies that
ADR-0001 removed and cannot test Toolbox behavior. A separate app keeps the
fault in the test process; Sherclawk owns orchestration and result
interpretation only.

This does not provide real isolation: a guest-wide crash or a non-yielding test
can still prevent observation and recovery. A timeout is an observation
deadline, not control over the test app. If OS 9 gains a stronger isolation
mechanism, or tests must run untrusted third-party code, revisit.

## Consequences

### Positive

- A failing test cannot corrupt Sherclawk's journal or session.
- Reuses snapshot, build, artifact-authority and launch machinery; no new
  execution path.
- Shared application logic is compiled into both the app and the test app,
  so tests exercise the code that ships.

### Negative

- The descriptor must grow a structured test target, and the current limit of
  five 4 KiB inputs is too small for the starter layout.
- A small C89 support library (named checks that record and continue, explicit
  finish) must be maintained and shipped in the starter.
- Cannot terminate a hung test app; Stop only ends observation.

### Risks

- Report forgery or accident: correlation and artifact checks prevent stale
  results but do not make project-written tests a trust boundary.
- Hung or crashed test app makes the guest unusable. Mitigation is a deadline
  that yields an `incomplete` outcome, never an automatic relaunch.

## Pros and Cons of the Options

### Separate native test application

- Good: fault containment at process level; reuses the verified build/launch path.
- Bad: launch configuration delivery has no Finder command line, so a custom
  initial Apple event or open-document event must be guest-verified.

### In-process execution

- Good: no launch or result protocol.
- Bad: harness lost on any test fault; no recovery of session state.

### Host-side test command

- Good: familiar CI shape.
- Bad: needs MacRelix or the host; cannot exercise Toolbox or File Manager.

## Implementation Notes

- Outcomes are reported separately from tool errors: `passed` (complete valid
  report, at least one check, zero failures), `failed`, `build_error` (nothing
  launched), `incomplete` (timeout, malformed or missing report, interrupted
  observation), and an explicit `no_tests` for an empty suite, never a pass.
- A fresh result folder per run. Completion is published only after all report
  data is written, closed and verified; a partial report may supply diagnostics
  but cannot pass. Never rely on a single global result path, on process
  disappearance, or on a result file merely existing.
- Uncertain observations are retained and never relaunched automatically.
- Checks record failures and continue; they must not depend on `assert()` or
  disappear under `NDEBUG`.
- Per-file size growth belongs with the editing work, not this decision.
- Overlaps issue #70 (correlated result support for diagnostics); share the
  result protocol where the shapes align.
- Revisit if configuration delivery cannot be made reliable in the guest.

## References

- ADR-0001
- #70
- Retro68's CTest and LaunchAPPL workflow is the precedent for launching a test
  app and collecting output; it is not a drop-in for native MPW builds.
