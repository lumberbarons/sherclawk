---
id: ADR-0008
title: Load folder guidance at startup and on demand
status: proposed
date: 2026-10-16
deciders: []
supersedes: [ADR-0004]
superseded-by: []
related: [ADR-0003, ADR-0007]
tags: [prompt, workspace, trust, limits]
---

# ADR-0008: Load folder guidance at startup and on demand

## Context and Problem Statement

ADR-0004 loads workspace-root `AGENTS.md` into the system message and guidance
from top-level "project" folders into history after their first tool round.
Workspace folders need not be projects, and instructions may live at any depth.
The current first-component detector mistakes root filenames for projects;
after eight names its table silently treats every later folder as already
checked. We want Claude Code's startup-plus-on-demand folder discovery model,
adapted to Sherclawk's bounded native runtime and explicit authority rules.

## Decision Drivers

- Any folder in the configured workspace may hold documents, source or other
  content; neither a `project.json` nor top-level placement establishes its
  right to contain guidance.
- A chat can work across sibling folders. Load their instructions as they
  become relevant without scanning the entire workspace at startup.
- Guidance discovered only after an edit cannot inform that edit. Newly
  discovered instructions must reach the model before affected mutations.
- Every history byte is uploaded again on later rounds. Static buffers,
  request/image headroom and the OS 9 application partition impose real bounds.
- File Manager work must remain cooperative and honor Stop, including on AFP.
  A cache cannot silently turn exhaustion into a claim that guidance was read.
- A handoff replaces history with a lossy summary. Relevant instructions must
  be loaded afresh rather than depending on the summary to preserve them.
- File guidance cannot widen tool permissions, outrank the user or bypass the
  journal, uncertain-outcome, build or process-ownership contracts.

## Considered Options

1. **Workspace guidance at startup, nested folder guidance on demand** — load
   applicable ancestors in order, with explicit scope and bounded delivery.
2. **Keep ADR-0004's top-level project scheme** — repair false entries but
   continue to ignore nested folder guidance.
3. **Load only the startup folder's ancestor chain** — Pi-style default
   discovery, without automatic loading when tools enter descendants.
4. **Load the entire workspace instruction tree at startup** — enumerate all
   folders and retain every instruction file before the first model request.
5. **Re-read applicable guidance before every request** — continuously refresh
   file contents, including the model's own edits, throughout a conversation.

## Decision Outcome

Chosen option: **Workspace guidance at startup, nested folder guidance on demand**.

This supersedes ADR-0004's delivery and discovery policy. It preserves bounded
owner guidance and the existing file/mutation boundaries, but removes the
project assumption, system-role placement of workspace text, and silent
"already seen" behavior at capacity. This document records a target design;
adding it does not change the running application's loader.

### Discovery and scope

- Use `AGENTS.md` in any workspace folder. The configured workspace is the
  discovery boundary under ADR-0007: never climb into its parents, sibling
  workspaces, the data root or the application installation.
- Read workspace-root guidance before the first model request of a new chat.
  Sherclawk has no separate shell working directory, so startup scope is the
  workspace root. Do not recursively scan its descendants for instructions.
- When a native file operation accesses a folder or a file within it, discover
  the applicable folder chain from the workspace root to that folder. A file
  uses its containing folder; a root-level file introduces no extra scope.
  Folder operations include the named folder itself.
- For `documents:letters:draft.txt`, the chain is root `AGENTS.md`,
  `documents:AGENTS.md`, then `documents:letters:AGENTS.md`. No descriptor is
  consulted to decide whether either folder qualifies.
- Native tools identify their actual workspace targets, including both sides
  of a copy and each item in a multi-file operation. Remote MCP arguments
  named `path` or `root` do not establish local workspace access.
- Recursive operations discover scopes as they actually access them, in
  bounded steps. Searching or listing must not become an unbounded instruction
  pre-scan of an otherwise untouched tree.
- Use validated workspace-relative colon paths and catalog identity to avoid
  duplicate delivery through equivalent spellings. Existing alias refusal and
  workspace containment still apply.

### Delivery, ordering and authority

- Deliver root and nested guidance as application-labelled user-role context
  in conversation history, separate from the built-in system policy. Include
  the source path and its folder scope. Guidance applies to that folder and
  its descendants; sibling guidance stays scoped even while retained in history.
- Within each newly discovered chain, deliver ancestors before descendants;
  an ancestor already delivered need not be repeated. Later visits do not
  remove previously delivered instructions or reorder the conversation.
- Keep the explicit hierarchy: built-in system rules and the user's requests
  outrank file guidance. Tool results and ordinary file content do not gain
  authority merely by referring to an instruction file.
- Concatenate applicable guidance rather than replacing whole ancestor files
  with child files. Folder ordering supplies context, not a mechanical
  conflict resolver. The model should surface material unresolved conflicts
  and obtain clarification when needed for the affected action.
- A read may complete with newly discovered guidance delivered before the
  next model request. A pending mutation, build publication or application
  launch must wait until the model has received relevant newly loaded guidance
  and can reconsider the action. Guidance arriving after publication does not
  satisfy this requirement.
- If this requires deferring a tool call, report that it was not executed and
  preserve valid assistant/tool pairing for the rest of the round. Never
  silently execute or replay the saved call after adding instructions; the
  model must choose the next action. Already completed effects remain recorded.

### Cache, limits and visibility

- Cache checked folders by full workspace-relative scope for the current
  history, distinguishing delivered, absent, unusable and deferred states.
  "Not loaded because a limit was reached" is never equivalent to delivered.
  Missing-file lookup caching must not consume the allowance for delivered
  instruction content or suppress unrelated folders.
- Freeze successfully loaded text for that history; do not reread every turn
  or silently activate the model's edit of an instruction file midway through
  the task. Explicit reads show current disk contents without pretending the
  automatic snapshot changed. A new history loads a fresh snapshot.
- Bound bytes per instruction file, aggregate delivered content, cache
  storage, path/depth handling and work per event turn together. Keep the
  current 4096-byte per-file UTF-8 allowance as the initial baseline, including
  its visible truncation marker; changing it requires rechecking request and
  partition budgets. Aggregate and cache sizes must be specified and measured
  before implementation is accepted, rather than inherited from the eight-name
  table or made unbounded.
- Report each loaded, truncated, unusable or limit-deferred file with its
  source and reason. An ordinary absent file may remain silent. If applicable
  guidance cannot be admitted, pause affected work with a visible explanation
  instead of proceeding as though it had been supplied. Preserve all work
  already completed; limits do not authorize cancellation or rollback.
- Journal the delivered context, source and revision so the recorded history
  explains what the model actually received. Report truncation honestly;
  do not describe a partial snapshot as the complete file.
- Continue to treat guidance as bounded plain MacRoman/CR workspace text,
  converted to UTF-8 for inference. Existing plain-text validation applies;
  this decision does not introduce a second text encoding or mutation path.

### New Chat, handoff and supported format

- New Chat and a successful handoff begin a new instruction-delivery epoch:
  clear folder-delivery state and load current root guidance before the first
  model request using the new history. Read nested guidance again on demand.
  A failed handoff leaves the old history and its instruction state intact.
- A summary may mention conventions with their original scope, but is neither
  the exact instruction snapshot nor proof that the fresh history has loaded
  it. Prevent summarized sibling guidance from being promoted to global rules.
- Keep existing guidance short and current through ordinary revision-guarded
  edits when the user's work changes what it describes. Do not create an
  instruction file unless asked.
- Adopt the discovery model, not Claude's entire configuration surface.
  `CLAUDE.md`, local/override filenames, `@` imports, path-glob rules, global
  user guidance and a dedicated reload command remain separate decisions.
  Explicit skill invocation and its handoff semantics are unchanged.

Arbitrary folder scope, work across siblings and delivery before effects tip
the balance. Startup-only ancestry misses folders entered later; eagerly loading
all descendants spends context and File Manager work on unrelated instructions.
Per-request refresh makes prompts unstable and adds repeated I/O. None removes
the need to define limits and show the user when guidance was not loaded.

## Consequences

### Positive

- Instructions can live with documents or code at any depth, without project
  identity or special handling of top-level folders.
- The model receives relevant ancestor guidance while unrelated branches stay
  unloaded until touched. Sources, scopes and exclusions are observable.
- Root and folder guidance share one authority framing and journalled delivery
  model. A handoff can recover current guidance without reconstructing it from
  the summary.

### Negative

- Folder-aware traversal, native tool target metadata and history admission
  require more controller work than extracting one argument's first component.
- New guidance can require another model round before a mutation. Long chains
  consume history and may require the user to shorten files or start a new chat.
- Truncation remains a compromise: a visible partial instruction file may omit
  an important rule. A future refuse-oversize policy would need a deliberate
  change rather than being implied by adopting Claude's model.
- Root content now consumes history and appears in the journal; its privacy and
  retention match other conversation content rather than metadata-only logging.

### Risks

- A missed native target, false cache hit or resumed deferred call can permit
  an action before the model sees its guidance. Exercise first-access mutation,
  multi-path and mixed read/write rounds; never treat a cache limit as success.
- A loaded sibling's instructions can influence unrelated work. Preserve source
  scope in each message and evaluate cross-folder tasks and handoffs.
- Agent-written or poisoned `AGENTS.md` remains owner-supplied context on later
  loads. Journal delivery, retain the explicit authority boundary, and preserve
  tool enforcement; prose is not a security sandbox.
- Extra snapshots or escaping expansion can exhaust static/history buffers.
  Reserve admission space before recording or executing, and measure the guest
  partition as well as host behavior.

## Pros and Cons of the Options

### Startup root plus on-demand folder ancestry

- Good: follows the folders actually used and respects arbitrary nesting.
- Bad: needs scoped caching, effect deferral and explicit capacity handling.

### Repair the top-level project scheme

- Good: smallest change to the existing implementation.
- Bad: still misses nested folders and preserves an artificial project boundary.

### Startup ancestry only

- Good: stable initial prompt and little discovery state.
- Bad: Sherclawk starts at the workspace root; sibling and nested guidance
  would require the model to find and read it manually.

### Eager discovery of every descendant

- Good: no first-access discovery delay after startup.
- Bad: unrelated instructions consume context, and workspace-wide traversal
  competes with bounded startup and Stop responsiveness.

### Refresh before every request

- Good: picks up disk edits immediately.
- Bad: repeated I/O and self-modifying instructions during a task; still needs
  per-call discovery for actions proposed after the request was assembled.

## Implementation Notes

- Replace `tools_call_project` and the first-component table with explicit
  native target discovery and host-testable folder-scope/admission logic.
  Toolbox adaptation stays in `src/app/main.c`; File Manager reads remain in
  the tools layer and advance cooperatively.
- `src/agent/agent.c` owns labelled context records and serialized admission;
  remove root-file text from `system_content`. `src/app/run_ctl.c` coordinates
  loading, pairing and any non-executed deferral before affected tool effects.
  `src/app/session.c` starts fresh delivery state only when a handoff commits.
- Preserve journal-before-effect barriers, Stop and transport drains. Neither
  adding guidance nor starting a new history resets native process ownership,
  cancels a published job, or permits retry of an uncertain operation.
- Validate root-file reads followed by real folder access; nested and sibling
  scopes; copy source/destination and multi-file targets; remote `path` keys;
  case-equivalent scopes; first-access mutations; failed instruction reads;
  capacity exhaustion; Stop; and successful/failed handoffs. Request snapshots
  must show file guidance in labelled history rather than the system policy.
- Update usage, architecture and limits when the behavior ships. Host fault
  checks and the full Docker build are necessary; verify loader/controller
  integration and responsiveness in OS 9.2.2 before claiming guest support.
- Revisit if first-access model rounds become a measured performance problem,
  users need larger or imported guidance, folder conflicts remain frequent,
  or provider/request storage changes make a different delivery model viable.
- Review assumptions: guidance is cached per history; first-effect calls may
  need a non-executed deferral; 4096 UTF-8 bytes remains the initial per-file
  allowance. Exact aggregate/cache budgets and the deferral protocol belong in
  implementation design. No decider handle was supplied for this draft.

## References

- [ADR-0004: Previous instruction-loading policy](0004-load-agents-md-as-bounded-owner-guidance.md)
- [ADR-0003: Tool admission and mutation guarantees](0003-admit-tools-read-only-first.md)
- [ADR-0007: Workspace and application-data boundaries](0007-keep-data-separate-from-application.md)
- [Claude Code: How CLAUDE.md files load](https://code.claude.com/docs/en/memory#how-claudemd-files-load)
  (consulted 2026-10-16): startup ancestry, lazy descendant discovery and
  concatenated guidance informed the chosen model; Sherclawk retains its own
  filename, authority boundary and native bounds.
- [Claude Code: What survives compaction](https://code.claude.com/docs/en/context-window#what-survives-compaction)
  (consulted 2026-10-16): root reinjection and on-demand nested restoration
  informed the fresh-history lifecycle.
- Maintainer conversation on 2026-10-15/16: folders need not be projects;
  prefer Claude-style discovery over startup-only ancestry; draft a replacement
  ADR before implementation.
