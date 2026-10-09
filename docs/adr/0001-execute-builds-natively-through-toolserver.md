---
id: ADR-0001
title: Execute builds natively through ToolServer Apple events
status: proposed
date: 2026-10-09
deciders: [lumberbarons]
supersedes: []
superseded-by: []
related: [ADR-0002]
tags: [build, toolserver, execution]
---

# ADR-0001: Execute builds natively through ToolServer Apple events

## Context and Problem Statement

`build_project` publishes an immutable snapshot as a job folder in the build
queue. Originally a Perl worker (`worker/worker.pl`) running inside MacRelix
claimed each job and ran its generated shell `script`, whose real work is
`tlsrvr` sending a "Do Script" Apple event to MPW's ToolServer. The queue
bookkeeping (exclusive folder create, marker rename, closed-file records,
`STOP` polling) was already native in `jobs.c`; only execution was external.
That left a human step (start MacRelix, keep it foregrounded because an idle
background worker stops polling), an out-of-band `ssh` step to create the
queue, and a worker file that had to be republished under a fresh name on every
change because of AFP caching. The question was how Sherclawk should execute
the MPW commands a build needs.

## Decision Drivers

- A Finder-launched classic app has no command line and cannot run `/bin/sh`
  text, so "execute the recipe script" is not something the app can do itself.
- MacRelix cannot be driven unattended. Driving it by Apple event is a trap:
  Genie's `execute` handler was deleted upstream.
- A delegated worker removes no manual step: a human still has to start and
  foreground MacRelix, so the build path stays non-self-contained.
- Builds take minutes on an emulated G3. The UI must stay responsive, so a
  blocking `kAEWaitReply` send (what `tlsrvr` does) is unacceptable.
- The harness contract requires explicit uncertain outcomes and no automatic
  replay. A shell script reports one aggregate exit code; per-command statuses
  and diagnostics are needed to report truthfully.
- MacRelix is AGPL-3.0. No code may be copied or linked; the protocol must be
  reimplemented from observed behavior.

## Considered Options

1. **Native ToolServer client** — Sherclawk sends MrC, PPCLink and Rez commands
   as asynchronous `kAEDoScript` Apple events and receives per-command replies
   in its own event loop.
2. **Keep the MacRelix Perl worker** — the app only publishes jobs and polls
   for results.
3. **Fold the queue loop into the app but still delegate execution to MacRelix.**

## Decision Outcome

Chosen option: **Native ToolServer client**.

It is the only option that removes the manual MacRelix step and the background
starvation problem, and the ToolServer channel is much smaller than it looks:
discover ToolServer by signature `'MPSX'`, send one script per command, read
`'stat'`, `'----'` and `'diag'` from the reply. Option 3 was rejected because it
keeps the dependency while adding code. Option 2 was kept only as a rollout
fallback and is being removed (#48).

Changing the drivers would invalidate this: if ToolServer stopped being
available in the supported guest, or if a supported way to run shell text
unattended appeared, the choice should be revisited.

## Consequences

### Positive

- Builds run with MacRelix absent; verified in OS 9.2.2 for success, compiler
  failure, and Stop (see `docs/history/verification.md`).
- Per-command raw status, mapped exit, stdout and diagnostics are recorded
  instead of one shell aggregate.
- The queue is still protocol-1 job folders, so logs, `read_build_log`
  continuation and artifact authorization are unchanged for the model.
- The same Apple-event machinery serves polite quit and open-document launch.

### Negative

- Sherclawk now owns Apple-event dispatch (`kHighLevelEvent`), reply correlation
  by return ID and sender PSN, and descriptor lifetime on every path.
- ToolServer and the MPW toolchain remain hard dependencies, and ToolServer may
  steal focus when launched.
- Artifacts are not byte-identical across rebuilds (PEF timestamp and resource
  map bookkeeping differ), so determinism cannot be asserted.

### Risks

- A late or lost reply leaves an in-flight command whose outcome is unknown.
  Mitigation: such outcomes are recorded as uncertain, never replayed, and late
  replies are drained without advancing an abandoned build.
- Background responsiveness is poor (a 5-second event gap was observed with
  ToolServer busy while Sherclawk was in the background), and behavior under
  window dragging or abrupt ToolServer death is unverified.

## Pros and Cons of the Options

### Native ToolServer client

- Good: self-contained; no human step; truthful per-command outcomes.
- Good: async send keeps the UI responsive.
- Bad: new Apple-event code to maintain; guest-only to verify.

### Keep the MacRelix Perl worker

- Good: already verified; no new app code.
- Bad: manual start, foreground requirement, republish-under-new-name ritual.
- Bad: one aggregate exit code; no per-command diagnostics.

### Delegate execution but fold in the queue loop

- Good: smaller than option 1.
- Bad: removes no manual step; the part that hurts stays.

## Implementation Notes

- `toolserver.c`/`.h` is a single-outstanding-command asynchronous client;
  `selfbuild.c`/`.h` is the executor integrated with `build_project`.
- Invariants future changes must respect:
  - One validated build representation generates both the shell recipe and the
    native steps, so they cannot drift.
  - Acquire the singleton `worker-lock` atomically before serving; never steal a
    stale lock. Switch executors only under exclusive ownership.
  - Send asynchronously. Stop ends observation; it does not prove cancellation.
  - Never replay a build after a timeout, lost reply or ToolServer
    disappearance, including the upstream out-of-memory retry-once behavior.
  - The executor's lifetime is independent of a chat run.
  - `started` records are unchanged; `native-executor` marks native ownership.
- Revisit if background latency becomes a product problem, or if byte-identical
  artifacts are ever required.

## References

- #48 (remove the MacRelix fallback), #65 (queue self-creation)
- ADR-0002 (tests reuse this executor)
- `docs/history/verification.md` for guest evidence
