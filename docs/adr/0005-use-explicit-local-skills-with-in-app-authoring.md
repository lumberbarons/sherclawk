---
id: ADR-0005
title: Use explicit local skills with in-app authoring
status: proposed
date: 2026-10-12
deciders: []
supersedes: []
superseded-by: []
related: [ADR-0003, ADR-0004]
tags: [prompt, workspace, trust, limits, skills]
---

# ADR-0005: Use explicit local skills with in-app authoring

## Context and Problem Statement

Sherclawk has standing workspace and project guidance through `AGENTS.md`,
but no library of named workflows a user can select for a particular task.
Users want to invoke a skill explicitly, see which skills are installed, and
ask Sherclawk to create or improve skills in the guest itself. The existing
workspace file tools can create folders and plain text, inspect references,
and perform revision-guarded edits; skill authoring does not require another
execution or mutation mechanism. The decision is how to make those workflows
discoverable and usable without making them permanent chat modes or expanding
what the harness can execute.

## Decision Drivers

- The user explicitly chose manual activation first and wants skills built
  in situ. Skill selection must be a user action, and authoring must work
  through the guest's existing workspace tools.
- `AGENTS.md` already supplies standing guidance. A selected workflow needs a
  task lifetime so it does not silently become another source of standing rules.
- `write_text` creates files only; `edit_text` requires a current revision and
  retains recovery evidence. An installer or skill editor must not bypass
  those contracts (ADR-0003).
- Sherclawk has cooperative scheduling, static buffers and a flat request
  rebuilt every model round. Currently the 32 KiB request overhead must cover
  a 16 KiB allowance for native policy and schemas, 4 KiB of workspace guidance
  and up to 8 KiB of frozen MCP schemas. Skill text and catalog metadata need
  explicit budgets; the remaining allowance is not an unrestricted skill arena.
- Handoff summarizes history and resets `Agent`. A selected skill must remain
  exact for a continuing task rather than relying on a lossy summary.
- Workspace text uses MacRoman/CR and colon-separated paths. Existing skills
  from other harnesses may instead assume UTF-8, Unix paths, shell execution or
  unavailable tools; the shared filename cannot imply execution compatibility.

## Considered Options

1. **Explicit local skill packages with a bundled authoring skill** — the app
   resolves a user-selected name, validates and loads its instructions, and
   offers a native inventory; the model authors workspace skills using the
   existing tools.
2. **Automatic selection from a catalog in every prompt** — the model chooses
   skills from descriptions and loads them as it judges appropriate.
3. **Put workflows in `AGENTS.md` or paste them into messages** — keep the
   existing guidance mechanisms without a skill library.
4. **Executable plugin packages and a dedicated installer** — skills register
   tools or scripts and are managed outside the existing text-file workflow.

## Decision Outcome

Chosen option: **Explicit local skill packages with a bundled authoring skill**.

### Activation and lifetime

- A skill activates only through an explicit user selection. The initial
  interface is a leading `@skill-name` prefix, with one skill per message;
  a native chooser inserts the same prefix. Mentions elsewhere are ordinary
  text. The app resolves the name before sending a model request, rather than
  asking the model to interpret the invocation.
- An unknown, ambiguous or unusable name leaves the message unsent with an
  explanation. Do not silently send the request without the selected skill.
- Read and freeze the complete validated instructions before the first model
  request. Show the selected name in the transcript and journal its source and
  revision. Edits take effect on the next invocation, never midway through the
  current task.
- Activation belongs to the selected task. Preserve it while that task is
  paused or explicitly continued, including across a handoff. Completion,
  abandonment, New Chat or a new independent task ends activation. Earlier
  conversation may retain skill-related text, but must not be presented as
  continuing activation. Define how continuation is selected before shipping;
  do not depend on an implicit permanent mode.
- Skills are owner-supplied workflow guidance under the same authority boundary
  as ADR-0004. They cannot outrank the user or harness rules, add tools, widen
  tool permissions, authorize uncertain retries or change build and launch
  authority. Explicitly loading a skill does not grant its reference files
  authority to activate further skills.

### Library and discovery

- Workspace skills are ordinary folders under one library location, initially
  `<workspace>Skills:<skill-name>:SKILL.md`. Each entry has a unique invocation
  name, short description and instruction body. Supporting text references may
  live beside it and be read as needed through existing bounded tools.
- The filesystem is the source of truth. Copying a valid package into the
  library or creating one there installs it; no separate installation database
  is required. Discovery and refresh are bounded and must honor Stop.
- A native Skills dialog lists names, descriptions, sources and usability.
  Invalid or conflicting entries remain visible with reasons. Its Insert
  action selects a usable skill for the message; installation alone does not
  activate anything.
- The initial format is a documented subset of `SKILL.md`: name and description
  metadata plus instructions and readable references. Packages do not execute
  scripts, register tools or install dependencies. External packages need
  adaptation to Sherclawk's tools, paths and text conventions.

### Authoring and validation

- Ship an inspectable authoring skill, initially named `write-skill`, available
  even in an empty workspace. Its delivery must not depend on an already
  working user skill library. The packaging or bootstrap mechanism is left
  to implementation; do not overwrite a user's package to install an update.
- A request such as `@write-skill Create a skill for diagnosing Toolbox events`
  uses `create_folder`, `write_text`, `read_text` and revision-guarded
  `edit_text`. Inspect the library first and improve an existing skill when
  that better matches the request. Verify the written files and make the
  resulting entry available after catalog refresh. Authoring or revising a
  skill never activates that newly written skill automatically.
- The authoring guidance teaches a clear purpose, expected input and output,
  useful decision rules, steps supported by actual tools, failure handling and
  observable completion. Keep the main workflow compact and move detailed
  references to separate files. Ask for missing domain knowledge instead of
  inventing procedures.
- The app validates package structure, names, required metadata, text,
  references and bounds. Refuse oversized instruction bodies rather than
  truncating a procedure. These checks establish usability, not quality.
- The authoring skill reviews substantive quality and, when practical within
  the requested work, exercises a representative task. Report what was tried
  and what remains unverified; a structurally valid package is not evidence
  that its workflow succeeds.

Explicit selection and reuse of existing workspace tools tipped the balance:
they meet the user's immediate needs without automatic prompt-wide catalog
selection or another mutation path. Automatic selection may be reconsidered
after explicit invocation is reliable and users identify a need for it.

## Consequences

### Positive

- Users can inspect, select, create and revise reusable workflows in OS 9.
- Skills remain ordinary workspace artifacts that existing file tools can
  verify and repair; no remote registry or host-side installer is required.
- A bundled authoring recipe provides a starting quality standard without
  putting every workflow into standing guidance.
- Selected instruction revisions and visible activation make it possible to
  explain which workflow governed a task.

### Negative

- Sherclawk must maintain catalog validation, a chooser and task activation
  state in addition to the existing `AGENTS.md` paths.
- Active instructions consume request space and input tokens every round;
  exact handoff preservation adds state that a summary alone cannot replace.
- Manual selection requires the user to know or discover the right skill.
- A subset format and classic text conventions limit direct reuse of packages
  written for other harnesses. Import conversion is not solved by this ADR.

### Risks

- A model can produce plausible but ineffective procedures. Mechanical checks
  must not label them proven; retain verification evidence and revise from
  observed failures.
- Incorrect lifetime handling can leave an old workflow active for an
  unrelated task or drop it during continuation. Make activation explicit in
  request construction and test completion, Stop, continuation and handoff.
- A copied or agent-written skill can contain conflicting guidance. Preserve
  harness precedence, expose source and revision, and never let package text
  extend tool authority.
- Partial authoring, duplicate names or edits during discovery can yield a
  misleading inventory. Show invalid entries and revalidate at invocation;
  failures must leave the request unsent.

## Pros and Cons of the Options

### Explicit local packages with bundled authoring

- Good: matches explicit activation and in-situ authoring; uses current tools.
- Good: the inventory is derived from inspectable files.
- Bad: requires a catalog, native UI, lifecycle state and a maintained authoring
  recipe; quality still depends on model judgment and observed outcomes.

### Automatic selection from a prompt catalog

- Good: users need not remember invocation names.
- Bad: introduces trigger ambiguity and catalog token cost before manual
  selection and skill quality have been demonstrated. Deferred for v1.

### Standing guidance or pasted workflows

- Good: needs no new package machinery.
- Bad: standing workflows lack task scope, while pasted workflows lack an
  installed inventory and a reusable authoring target.

### Executable plugins and dedicated installation

- Good: could deliver capabilities beyond instructions.
- Bad: changes the execution and mutation boundary, adds installation state,
  and assumes facilities absent from this native app. Requires a separate
  architectural decision if pursued.

## Implementation Notes

- Keep parsing, catalog validation and lifecycle logic host-testable. Toolbox
  glue in `main.c` handles selection and workspace File Manager access; do not
  put all skill logic into the UI/event loop.
- `agent.c` must frame selected instructions as task guidance and account for
  their serialized size. `session.c` must preserve the frozen instructions
  needed to continue an active task, separately from the lossy summary, and
  retain the distinction between audit history and current activation.
- Choose numeric limits for entries, names, descriptions, instructions and
  retained activation state together with `docs/limits.md`. Account for UTF-8
  conversion, JSON escaping, history admission, MCP schemas and images, plus
  static copies and guest heap headroom. Do not assume the current instruction
  loader's truncation policy is suitable for skills.
- Workspace packages follow existing plain TEXT, MacRoman/CR, relative colon
  path and alias-refusal rules. Relative reference names must resolve within
  the intended package through workspace validation, never host paths.
- Use a documented metadata subset; a full YAML parser is not implied. Resolve
  collisions between bundled and workspace entries explicitly; do not silently
  shadow the authoring skill or select whichever file was scanned first.
- Runtime documentation must explain installation, selection, invalid entries,
  activation lifetime, authoring and compatibility limits when implemented.
  Host fault tests and an OS 9.2.2 diagnostic must verify the relevant loader,
  UI and persistence behavior before claiming support.
- Review assumptions: the leading `@` syntax, single library location,
  one-skill-per-message rule, `write-skill` name and chooser insertion behavior
  are initial design details proposed here. Exact metadata grammar, budgets,
  continuation UI, bundled delivery and import conversion remain open. No
  decider handle was supplied; record it when accepting this ADR.
- Revisit when users need combined or automatic selection, larger instructions,
  multiple library scopes, external import, or executable packages, or when
  measured guest/request costs make the chosen lifecycle impractical.

This complements ADR-0003 and ADR-0004 and supersedes neither. It records the
target design; skills are not implemented by adding this document.

## References

- [ADR-0003: Admit platform tools read-only first and journal every mutation](0003-admit-tools-read-only-first.md)
- [ADR-0004: Load AGENTS.md as bounded owner guidance](0004-load-agents-md-as-bounded-owner-guidance.md)
- [Tool contracts](../tools.md), [project guidance and handoff usage](../usage.md),
  and [request and memory limits](../limits.md).
- Maintainer conversation on 2026-10-12: explicit activation first, installed
  skill discovery, and authoring good skills in situ.
