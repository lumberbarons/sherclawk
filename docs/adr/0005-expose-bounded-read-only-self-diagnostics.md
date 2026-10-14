---
id: ADR-0005
title: Expose bounded read-only self-diagnostics
status: proposed
date: 2026-10-12
deciders: []
supersedes: []
superseded-by: []
related: [ADR-0001, ADR-0002, ADR-0003, ADR-0004, ADR-0007]
tags: [diagnostics, tools, journal, limits]
---

# ADR-0005: Expose bounded read-only self-diagnostics

## Context and Problem Statement

Sherclawk should be able to investigate its own failed or slow runs using
evidence from the native application. Today, `get_environment` exposes OS,
workspace, capabilities and heap headroom; `src/app/main.c` logs network and tool
timings and stop reasons; and `src/app/session.c` preserves conversation and execution
records. However, the lifecycle log is hard-coded to `Retro68:Sherclawk.log`
and recreated on launch, runtime state has no dedicated model-facing reader,
and UTF-8 journals cannot be decoded reliably through the MacRoman `read_text`
contract. Existing guest diagnostics establish behavior but do not give a
running Sherclawk a coherent account of its own incidents.

## Decision Drivers

- When inference is unavailable, the model cannot investigate through tool
  calls. Evidence collection and local report export must still work without
  inference, an API key, MCP or a host executor, so the evidence is available
  for model-assisted investigation when inference is working again.
- Relaunch is a normal response to a classic Mac OS fault. Startup must preserve
  the previous launch's diagnostic evidence within a bounded retention policy.
- Application installation, the chosen data root and the active workspace are
  separate under ADR-0007. Diagnostics must work without a `Retro68:` volume
  and may record an unavailable network workspace in an available local data
  root; they must not silently choose a different store when that root fails.
- OS 9 uses cooperative scheduling and has no memory protection. Diagnostic
  reads, writes and enumeration must advance in bounded steps and honor Stop;
  the app cannot promise to diagnose a guest-wide hang from inside that hang.
- Model-facing JSON results fit `AGENT_RESULT_CAP` (currently 4096 bytes).
  Conversation/request buffers and the application partition are bounded;
  diagnostic history must not silently expand either.
- Memory observations can mislead: `FreeMem` reports the grown zone's slack,
  while `get_environment` adds the ungrown gap to report headroom. Calling
  `MaxApplZone` to measure it caused a guest-verified Preferences crash.
- Session journals contain user and file contents. Credential files are
  protected by catalog identity. Self-diagnosis must expose useful operational
  evidence without turning either into a general model-visible dump.
- Build locks, process presence and missing completion records are observations,
  not proof of liveness, successful execution or cancellation. Diagnosis must
  preserve the existing no-replay and process-ownership contracts.

## Considered Options

1. **Bounded native diagnostic state and retained events** — expose read-only
   observations to the model, backed by the same evidence used for local report
   export.
2. **Use existing file tools and add logging hints to the prompt** — ask the
   model to find and interpret lifecycle logs and raw session journals.
3. **Rely on a host or remote diagnostic service** — make an external observer
   the primary source of diagnosis.
4. **Give the model a general repair interface** — expose shell/script execution,
   arbitrary preferences, memory inspection or automatic restart/retry controls.

## Decision Outcome

Chosen option: **Bounded native diagnostic state and retained events**.

Provide three complementary surfaces: a current state snapshot, a paginated
reader for retained diagnostic events, and a local command to save a report
without calling the model. The snapshot and event reader are read-only tools;
report export is an explicit application-owned file publication. Collection
continues independently of chat history and New Chat, and evidence remains
available after relaunch subject to explicit storage bounds.

Sherclawk uses inference and calls the diagnostic tools to investigate itself:
the model reads current state and retained events, follows continuations,
correlates observations and explains likely causes and remaining uncertainty.
The requirement to work without inference applies to evidence collection and
local report export, not to this model-assisted investigation. Retained evidence
also lets a later working model session investigate an earlier failed run.

Investigation starts only when the user asks; diagnostic collection continues
automatically in the background. A successful investigation explains the failure
and cites supporting evidence, including uncertainty and missing observations.
It does not automatically begin repairs or retry failed work. The initial scope
covers failed or slow agent runs (network, model responses, tools, journals,
builds and MCP), plus investigation of an earlier app crash or freeze after
relaunch. UI and interaction faults are outside this initial scope.

During a requested investigation, Sherclawk may supplement retained evidence
with bounded read-only checks, such as checking workspace readability and
ToolServer process presence. Investigation does not itself authorize test model
or MCP requests, write-based probes or other reproduction actions. Diagnostic
recording is best-effort: if it fails while the mandatory session journal still
works, ordinary use continues with a visible warning about incomplete evidence.

The state snapshot and retained timeline are the first priorities. Together
they let Sherclawk distinguish a timeout, malformed response, journal failure,
memory allocation failure or outstanding build from evidence. Local export
provides access when inference itself is broken. Prefer a concise architecture
and troubleshooting guide matched to the running build, read in bounded sections
on demand. The guide is optional: when absent, oversized or unavailable,
investigation proceeds from operational evidence. Bundling source files is
outside the initial scope; the guide is not embedded in every model request.

The deciding drivers are loss of evidence on relaunch, inference-dependent
access and the encoding/privacy limits of raw journals. More prompt advice
does not resolve those limits. External observers remain useful for freezes,
crashes and visual acceptance, but cannot be a requirement for ordinary
self-diagnosis on a real Mac. General repair controls widen authority without
first improving the evidence and are outside this decision.

## Consequences

### Positive

- The model and a developer can investigate the same correlated operational
  evidence, including incidents from an earlier launch.
- Network, tool and storage failures remain inspectable when inference is
  unavailable; the next working session can read retained evidence.
- Read-only diagnosis reuses native tool admission and limits without granting
  new mutation, build, launch or Quit authority.

### Negative

- State adapters, event schemas, retention and report publication add code and
  File Manager I/O to a memory-constrained native app.
- Bounded retention deliberately loses older evidence; omitted fields and
  truncated history must be visible to the reader.
- A separate operational record duplicates some journal information. Shared
  event producers must keep the two accounts consistent without copying private
  conversation payloads.
- A matched implementation guide requires build identity and release packaging
  discipline; explanations may remain limited to operational evidence when the
  optional guide is unavailable, and the initial scope does not bundle sources.

### Risks

- Diagnostics may alter the fault they are meant to observe. Use passive memory
  measurements, sparse events and bounded buffers; measure guest heap and
  responsiveness with collection enabled.
- Free-form errors, URLs or remote replies may contain secrets. Prefer fixed
  error codes and allowlisted fields, sanitize any necessary text before storage,
  and test reflected credentials as well as direct configuration access.
- A disk or share failure can prevent recording. Surface unavailable, partial
  or failed evidence explicitly; never report an empty record as a healthy run.
  Diagnostic-store failure alone does not block ordinary use, so an incident
  may remain unexplained despite successful execution-journal recording.
- A missing end event may mean a crash, force-stop, lost mount or write failure.
  Report an incomplete launch record, not an inferred crash cause.
- The diagnostic read itself changes current tool counts and timings. Label
  observation time and distinguish the current inspection from the prior
  incident; do not recursively log reads of diagnostic events as new incidents.

## Pros and Cons of the Options

### Bounded native diagnostic state and retained events

- Good: works with native files and tools; preserves earlier incidents; supports
  local export without a network request.
- Bad: introduces persistence and retention policy that must be tested against
  real File Manager behavior.

### Existing file tools and logging hints

- Good: minimal new machinery; existing tools remain useful for ordinary app logs.
- Bad: cannot expose live internal state or fix startup deletion, UTF-8 decoding
  and private journal payloads; requires inference for the investigation.

### Host or remote diagnostic service

- Good: can observe a stopped or wedged guest and collect screenshots externally.
- Bad: requires infrastructure unavailable on a standalone Mac and may depend
  on the same network that failed; does not replace native evidence collection.

### General repair interface

- Good: would permit more recovery actions from one conversation.
- Bad: arbitrary scripts, memory access or restart/replay can destroy evidence
  and bypass existing authority boundaries. A specific repair capability needs
  its own design and admission review after diagnosis is reliable.

## Implementation Notes

- Keep diagnostic storage, paging and formatting separate from Toolbox UI code,
  with host-testable interfaces. `src/app/main.c` supplies run state; `src/agent/network.c` and
  `src/app/timing.c` supply exchange observations; the MCP controller and native build
  machinery supply their own state. `src/agent/registry.c` remains pure tool data.
- A snapshot identifies the app version and exact build, including whether it
  has uncommitted changes. It reports model/effort, run state and counters,
  history usage and limits, passive heap headroom, last failure, last network
  phase/status, journal and diagnostic-store health, ToolServer/outstanding
  build observations and MCP discovery/call state. Unavailable observations
  stay unknown; advertised capability is distinct from current availability.
- Events carry a schema version, launch identity, sequence and timestamp/tick
  information, plus applicable chat/run/round/tool/build identifiers. Capture
  significant state changes, errors, Stop and terminal outcomes; avoid logging
  every idle or draw event. Preserve existing per-phase timing semantics.
- Use a dedicated UTF-8 reader for diagnostic records. Continuations bind to
  retained launch identity and stable event positions; report stale cursors,
  eviction and partial records explicitly. Do not reinterpret raw UTF-8
  conversation journals as MacRoman or expose them wholesale through this API.
- Retain records in the chosen Sherclawk data root's application-owned `Logs:`
  area under ADR-0007, independently of the installed application's location
  and the active workspace. The main lifecycle log and retained launch events
  share that area; the main log must not be the sole retained evidence if it
  is replaced on launch. Default local report exports to the data root's
  `Reports:` folder. Neither location requires the literal `Retro68:` volume.
  Identify the applicable workspace by its stable ID on workspace-specific
  events; a launch record can span idle workspace switches without rebinding
  earlier events. An open record keeps its storage location when preferences
  change; data-root changes require the quiescent transition in ADR-0007.
  There is no automatic fallback or mirror when the selected data root is
  unavailable. Report that storage limitation explicitly. A missing network
  workspace does not prevent recording or report export if the selected data
  root remains available, but workspace observations must report unavailability.
  Keep a few recent launches for comparison and a bounded history of failed or
  incomplete launches, within the total storage cap.
  Startup never truncates the previous record; app-managed eviction is
  restricted to identified diagnostic records and never prunes session
  journals, build evidence or MCP result artifacts.
- Define hard limits for event size, in-memory buffers, read/write work per
  event turn, records per launch and total retained launches/bytes before
  implementation is advertised. Record retention loss visibly, protect the
  active record, and document memory costs in the limits guide. Exact caps,
  record filenames and tool names are implementation choices; ADR-0007 governs
  the data-root layout.
- Protect diagnostic evidence from ordinary source mutations. Only the
  diagnostic recorder/retention code owns writes and eviction. Explicit local
  report export uses create-only publication with bounded content and truthful
  success/failure; it needs no inference, MCP service or host executor, but does
  require the selected data root to be available. It cannot capture new
  observations while the app or guest is unable to service events.
- Operational diagnostics exclude API keys, authentication headers, private
  configuration contents, prompts, tool arguments, file contents and remote
  response bodies. If operational evidence is insufficient, Sherclawk may
  request explicit authorization to inspect relevant conversation-journal
  records through a separate reader. A request to diagnose does not itself
  authorize that access. Authorization is scoped to the relevant records for
  that investigation and does not require repeated confirmation within that
  scope; credentials remain protected. A diagnostic recording failure leaves
  ordinary use enabled with a visible warning and an unhealthy diagnostic-store
  observation. It must not weaken mandatory journal barriers: if an execution
  journal fails, execution still stops.
- Observations never authorize stealing locks, replaying uncertain work,
  restoring process ownership, force-quitting apps or changing preferences.
  Passive diagnosis does not execute project-authored test code inside
  Sherclawk; separate diagnostic apps retain the containment rules of ADR-0002.
- Supply the optional architecture and troubleshooting guide as bounded,
  version-matched text read on demand. Missing or oversized guide content never
  blocks diagnosis from operational evidence. Diagnostic results and reference
  text are evidence, not instructions that can widen tools or override policy.
- New model-facing tools require host fault checks, the Docker app build and
  recorded OS 9.2.2 verification before advertisement. Exercise relaunch,
  non-ASCII events, unavailable inference/storage, retention boundaries, Stop
  and reflected credentials, as well as successful reads.
- Revisit if native evidence cannot explain recurring faults, collection
  materially harms event responsiveness or heap headroom, or requirements grow
  to crash dumps, arbitrary repair or remote telemetry. Those need a separate
  decision rather than silently expanding this interface.

## References

- [ADR-0001: Native ToolServer execution](0001-execute-builds-natively-through-toolserver.md)
- [ADR-0002: Separate native test applications](0002-run-tests-in-separate-native-app.md)
- [ADR-0003: Read-only admission and journaled mutations](0003-admit-tools-read-only-first.md)
- [ADR-0004: Bounded owner guidance](0004-load-agents-md-as-bounded-owner-guidance.md)
- [ADR-0007: Separate application and data folder](0007-keep-data-separate-from-application.md)
- [Tool contracts and protected credential files](../tools.md)
- [Session files](../usage.md#session-files)
- [Memory partition and measurement constraints](../limits.md#memory-partition-hellor)
- [Guest verification history](../history/verification.md), especially the
  application heap headroom and Preferences crash investigations.
- [Build queue status proposal](https://github.com/lumberbarons/sherclawk/issues/121)
- [Unattended native diagnostics](https://github.com/lumberbarons/sherclawk/issues/73)
- [Native observation and action](https://github.com/lumberbarons/sherclawk/issues/80)
