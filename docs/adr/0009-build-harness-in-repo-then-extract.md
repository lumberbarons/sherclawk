---
id: ADR-0009
title: Build the OS 9 guest harness in-repo, then extract it
status: accepted
date: 2026-10-18
deciders: [lumberbarons]
supersedes: []
superseded-by: []
related: [ADR-0001, ADR-0002, ADR-0007]
tags: [testing, build, tooling, repository]
---

# ADR-0009: Build the OS 9 guest harness in-repo, then extract it

## Context and Problem Statement

The reserved-guest harness (hew epic #67 and its children) turns the current
manual OS 9 QEMU testbed into a host manager with guest reservation, QMP
driving, publication into private per-run workspaces, an in-guest controller,
and an unattended diagnostic suite. Part of this harness already exists in this
repository: `tools/qmpdrive.py`, `tools/utm_qmp.py`, `tools/guest-input.py`,
`tools/netatalk_meta.py`, and the setup knowledge in `docs/macos9-qemu.md`.
The manager is meant to be installed per host, outside any worktree, and other
projects beyond Sherclawk need the same harness for their own guests and
diagnostics.

The decision is where this harness lives while it is being built: grown inside
this repository and extracted once proven, or started in its own project
immediately.

## Decision Drivers

- Sherclawk's diagnostics are the only existing harness consumers; proving the
  harness against them requires sharing this repo's build and CI today. The
  guest controller (#70) is a native PowerPC application built through this
  repo's CMake diagnostic targets, and #73 migrates the existing guest checks
  onto its report contract.
- CI gates run without Docker, a VM or API keys (`tools/check.sh` on
  `ubuntu-24.04`, clean under GCC and clang; `tools/lint.sh`). The harness's
  fake-QMP and report suites (#74) are stdlib-only and are designed to fit
  these gates; a standalone repo would need to duplicate them before any proof
  exists.
- Nearly all harness code is greenfield (epic #67 is 0/7). Extraction later is
  cheap only if boundaries are kept from day one; a split today would force
  two-repo contract versioning while the request/config/report contract (#70)
  is still being established.
- #71 places the manager outside worktrees at run time. Source location and
  install location are independent: harness code can live in a repository
  while the manager installs host-wide, so in-repo development does not block
  multi-project use once it is proven.
- A proven pattern for the later dependency already exists: Certainly is
  fetch-only, cloned to a sibling directory with a `CERTAINLY_DIR` override,
  never committed, and absent checkouts degrade the affected suites instead of
  failing unrelated gates.
- Keeping the harness inside Sherclawk permanently would couple other projects
  to this tree and force them to copy or reach into a project's source to test
  their own guests.

## Considered Options

1. **Build in-repo first, extract after proven** — grow a self-contained
   harness directory mirroring the future standalone layout, prove it through
   Sherclawk's real diagnostics and CI, then extract.
2. **Standalone repository from day one** — create the harness project now and
   have Sherclawk depend on it immediately.
3. **Keep the harness inside Sherclawk permanently** — other projects copy the
   code or reach into this repository's tree.
4. **Extract only the generic QMP layer now; build the rest in-repo** — split
   `tools/qmpdrive.py`/`utm_qmp.py` into a shared project immediately and grow
   the remainder here.

## Decision Outcome

Chosen option: **Build in-repo first, extract after proven.**

Develop the harness in a top-level directory (for example `harness/`) that
mirrors the layout of the future independent repository, with no imports from
`tools/` or `src/`. Prove it by running Sherclawk's real diagnostics through
it, using the existing build and CI gates. Once proven, extract it to its own
project; Sherclawk then depends on the harness through a documented, fetch-only
public interface (for example a `HARNESS_DIR` checkout, mirroring
`CERTAINLY_DIR`), and guest-build-dependent suites degrade explicitly when it
is absent, as check-transport does today for Certainly.

The balance: proof requires one shared build/CI tree with the only real
consumer, and the contract is still churning (#70), so a split today buys
two-repo overhead before any of it is earned. Extraction is cheap precisely
while the code is greenfield, provided the boundary is enforced from the first
commit — which is a discipline risk, not a technical one, and is addressed in
Implementation Notes.

## Consequences

### Positive

- One shared tree builds the controller, the diagnostics and the harness tests
  during development; contract churn stays in one repo.
- The harness is proven against its real consumer before it is frozen into a
  standalone project.
- CI inherits the existing host gates; the harness's fake-QMP suites (#74) run
  without a VM from day one.
- Extraction becomes a mechanical move of the directory plus new CI, with a
  proven precedent (Certainly) for the dependency mechanics.

### Negative

- Boundary discipline is the whole burden: every import from `tools/`/`src/`
  into harness code, or harness-internal dependency from Sherclawk consumers,
  silently raises extraction cost.
- `check.sh`, `lint.sh` and `CMakeLists.txt` must keep harness sources separate
  from app sources from day one, adding ongoing review obligations.
- Deferral risk: "prove it" can feel perpetually unfinished, so extraction must
  be an explicit deliverable rather than an aspiration.

### Risks

- If the harness needs Sherclawk-only headers or APIs before the proof
  completes, or two consumers require different contract versions, the phasing
  must be revisited (leading indicators below).
- The manager is installed outside worktrees while its source lives in a
  repository; consumers need a documented clone/install path or they will
  vendor copies that drift.
- Naming and ownership: the future project needs its own identity, repository
  and license posture; guest disk images, machine-local credentials and
  artifacts stay out of git in both phases.

## Pros and Cons of the Options

### Build in-repo first, then extract

- Good: shared build/CI during proof; greenfield timing keeps extraction
  cheap; proven dependency pattern exists.
- Bad: requires enforced boundaries and an explicit extraction deliverable.

### Standalone repository from day one

- Good: cleanest long-term ownership; no rebranding or move later.
- Bad: two-repo contract versioning while #70's contract churns; harness CI
  must be duplicated before any proof; changes to the controller cross repos
  mid-flight.

### Keep the harness inside Sherclawk permanently

- Good: no move, no new project.
- Bad: other projects must copy code or reach into this tree; the manager
  belongs to the host, not to one consumer project.

### Extract only the generic QMP layer now

- Good: moves the least controversial code immediately.
- Bad: the QMP layer's shape is dictated by the manager's reservation and
  proxy design (#71/#69); splitting it before those exist would guess the
  interface and likely split again.

## Implementation Notes

- Create a top-level `harness/` directory mirroring the future standalone
  layout: stdlib-only Python host code with its own tests, the native guest
  controller and report-contract support, and its own provisioning
  documentation. It must not import from `tools/` or `src/`.
- Keep harness sources separated from app sources in `check.sh`, `lint.sh` and
  `CMakeLists.txt` from day one; harness code builds through shared CMake
  targets until extraction, but only as an unambiguous source grouping, never
  as shared app headers or helpers.
- Sherclawk consumers (diagnostic checks, `deploy-to-share.sh`) depend only on
  the harness's public CLI and versioned contract, never its internals.
- The harness adds its own fake-QMP and report suites to `check.sh` without
  requiring a VM or API key; host gates stay the CI gate throughout.
- Resist putting Sherclawk-specific logic (tool names, journal formats,
  workspace policy) into harness helpers; that belongs in the consumers.
- After extraction, the harness repo owns the manager, QMP layer, provisioning,
  controller and suite runner; Sherclawk keeps its diagnostics and gains a
  documented clone/install path for the harness (like Certainly's clone line)
  with explicit degradation when the checkout is absent.
- Extraction is tracked as an explicit issue with its own done-when: a fresh
  clone of the standalone project runs the deterministic suite against a fresh
  reservation, and Sherclawk's gates pass with the harness provided externally.
- Revisit this decision if: harness code needs Sherclawk-only headers or APIs;
  two consumers need different contract versions before the proof completes;
  or provisioning turns out to be consumer-specific rather than harness-owned.

## References

- AGENTS.md: Certainly fetch-only dependency and gate degradation
- `docs/macos9-qemu.md`: original QEMU testbed setup
- hew #67 (epic: Reserved QEMU guests and unattended native diagnostics) and
  children #68–#74
- hew #247 (extract the harness into a standalone project; the explicit
  extraction deliverable)
- ADR-0001: native ToolServer execution (controller builds through the same
  toolchain)
- ADR-0002: tests run in a separate native application (report-contract origin)
- ADR-0007: data separate from the application (manager state outside worktrees)
