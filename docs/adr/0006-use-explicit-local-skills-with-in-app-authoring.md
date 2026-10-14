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
- `AGENTS.md` already supplies standing guidance. Explicit skill invocation
  adds workflow instructions to conversation context; later user messages
  determine how they apply without a separate app-managed task mode.
- `write_text` creates files only; `edit_text` requires a current revision and
  retains recovery evidence. An installer or skill editor must not bypass
  those contracts (ADR-0003).
- Sherclawk has cooperative scheduling, static buffers and a flat request
  rebuilt every model round. Currently the 32 KiB request overhead must cover
  a 16 KiB allowance for native policy and schemas, 4 KiB of workspace guidance
  and up to 8 KiB of frozen MCP schemas. Skill text and catalog metadata need
  explicit budgets; the remaining allowance is not an unrestricted skill arena.
- Handoff summarizes history and resets `Agent`. The user chose to carry skill
  context through that ordinary summary, accepting the same lossiness as other
  conversation context rather than adding skill-specific persistence.
- Workspace text uses MacRoman/CR and colon-separated paths. Existing skills
  from other harnesses may instead assume UTF-8, Unix paths, shell execution or
  unavailable tools. The user requires support for well-formatted packages
  following the Agent Skills specification; its package format must be
  supported while runtime capabilities remain those of Sherclawk.

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

### Invocation and conversation context

- A skill activates only through an explicit user selection. The initial
  interface is a leading `@skill-name` prefix. The first version accepts one
  skill per message; combined invocation is a future option. A native chooser
  inserts the same prefix. Mentions elsewhere are ordinary text. The app
  resolves the name before sending a model request, rather than asking the
  model to interpret the invocation.
- An unknown, ambiguous or unusable name leaves the message unsent with an
  explanation. Do not silently send the request without the selected skill.
- Read and freeze the complete validated instructions before the first model
  request. Show the selected name in the transcript and journal its source and
  revision. Edits take effect on the next invocation, never midway through the
  current task.
- Invocation adds the validated instructions to conversation context, labelled
  with their source and the user's selection. Later user messages determine
  whether to continue, change or stop following the workflow. An unprefixed
  follow-up requires no special continuation or deactivation action; the model
  interprets it using the context and the current user request. Do not add an
  app-managed active-skill mode or infer task completion to expire instructions.
  New Chat clears this conversation context.
- Handoff carries only the ordinary conversation summary: what was done or is
  intended, why, and which skills were used when relevant to that account. It
  does not preserve the skill procedure. Do not save a separate instruction
  snapshot or automatically reload skills named in a summary. A fresh explicit
  invocation loads the installed skill again.
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
- Custom skills belong to the currently configured workspace. The first
  version uses no shared global library or cross-workspace fallback. Discovery
  and name resolution use the current workspace, including its authoring
  override. A shared global library remains a future option; adding it would
  require explicit scope and name-precedence rules.
- The filesystem is the source of truth. Copying a valid package into the
  library or creating one there installs it; no separate installation database
  is required. Discovery and refresh are bounded and must honor Stop.
- A native Skills dialog lists names, descriptions, sources and usability.
  Invalid or conflicting entries remain visible with reasons. Its Insert
  action selects a usable skill for the message; installation alone does not
  activate anything.
- Explicit invocation needs no model-facing catalog: the app resolves the
  selected name and supplies only that skill's instructions. Do not send the
  installed names and descriptions in every prompt. The authoring workflow
  can inspect the workspace library using existing file tools when needed.
- Follow the Agent Skills specification for package structure and `SKILL.md`
  format: YAML frontmatter, required name and description, defined optional
  fields, Markdown instructions and package-relative resources. Support
  well-formatted external packages rather than defining a Sherclawk-specific
  format subset. The authoring skill produces conforming packages too.
- Format validity and runtime compatibility are distinct. Optional scripts,
  references and assets may be present in a conforming package; their presence
  does not register tools, install dependencies or create a new execution path.
  Experimental `allowed-tools` metadata cannot grant tools or authority beyond
  Sherclawk's existing harness.
- Load a valid, readable skill even when it describes requirements Sherclawk
  lacks, such as Python or a shell. Supply its compatibility information with
  the instructions and let the model assess the task against the actual tools.
  It must explain unavailable capabilities, perform only the parts it can
  support, and report when the requested outcome cannot be achieved. Never
  pretend to run a script or substitute an unsupported execution path. Package
  validity, read failures and enforced size/path limits still govern whether
  instructions can be loaded; do not infer a compatibility blocker from prose.

### Authoring and validation

- Ship an inspectable authoring skill, initially named `write-skill`, available
  even in an empty workspace. Its bundled default remains available for
  reference. When the user asks to customize the authoring guidance, create a
  workspace copy through the existing file tools. That copy takes explicit
  precedence over the bundled default, and the chooser identifies which source
  is selected. Application updates preserve the workspace copy. An invalid
  override is reported and blocks invocation; do not silently fall back to the
  bundled default. The bundled skill's delivery must not depend on an already
  working user skill library.
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
  and applicable bounds. Validate referenced resources when accessed through
  the relevant tools; the presence of scripts or other unsupported resources
  does not invalidate the instruction file. Refuse oversized instruction
  bodies rather than truncating a procedure. These checks establish loadability,
  not quality.
- A structurally valid authored skill becomes immediately available to invoke
  after catalog refresh. There is no draft-enablement or representative-trial
  gate. Availability does not activate it; verification evidence is reported
  so the user can judge its quality.
- The authoring skill reviews substantive quality and, when practical within
  the requested work, exercises a representative task. Report what was tried
  and what remains unverified; a structurally valid package is not evidence
  that its workflow succeeds.

### Evidence of usefulness

- A skill can be ineffective or make results worse than a plain request.
  Structural validity and a successful trial do not establish that it adds
  value. Authoring guidance should help the user assess that question, including
  proposing representative prompts and concrete success criteria when useful.
- Compare the same task with and without the skill in fresh conversations,
  using the same model settings and equivalent starting inputs. The authoring
  conversation already contains the procedure and cannot serve as a clean
  baseline. New Chat does not undo file mutations; use separate equivalent
  fixtures for tasks that change files.
- Judge correctness and usefulness first, then effort and cost where evidence
  is available. Record regressions and inconclusive results as well as gains;
  revise or simplify the skill when the comparison warrants it. Claims of
  improvement must be limited to the cases actually compared.
- The first version may guide manual comparisons through existing chats and
  files. It does not require an automated evaluation runner, isolated model
  workers or automatic grading. Evaluations are optional and do not gate
  availability; automated evaluation remains a future option.

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
  putting every workflow into standing guidance, and a workspace override lets
  users refine that recipe while retaining the original for reference.
- Selected instruction revisions and visible activation make it possible to
  explain which workflow governed a task.

### Negative

- Sherclawk must maintain catalog validation, a chooser and instruction
  delivery in addition to the existing `AGENTS.md` paths.
- Loaded instructions consume history and input tokens every round, and their
  full procedure can be lost when a handoff summarizes the conversation.
- Manual selection requires the user to know or discover the right skill.
- Supporting standard YAML frontmatter and external text conventions adds
  parser and loader work. A valid package can still require unavailable
  runtime capabilities; standards compliance cannot supply those capabilities.

### Risks

- A model can produce plausible but ineffective procedures. Mechanical checks
  must not label them proven; comparisons can reveal added cost or worse
  results. Retain verification evidence and revise from observed failures.
- The model may overapply an earlier workflow or lose it during a handoff.
  Frame instructions under current user intent, verify behavior on follow-ups
  and changed requests, and document that a handoff does not preserve the full
  skill. Users can explicitly invoke it again when exact instructions matter.
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
- Bad: requires a catalog, native UI, instruction delivery and a maintained
  authoring recipe; quality still depends on model judgment and observed outcomes.

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

- Keep parsing, catalog validation and instruction delivery host-testable.
  Toolbox glue in `src/app/main.c` handles selection and workspace File Manager access;
  do not put all skill logic into the UI/event loop.
- `src/agent/agent.c` must frame selected instructions as user-requested guidance in
  conversation history and account for their serialized size. `src/app/session.c`
  records their delivery and uses the existing summary-based handoff. No
  active-task state or skill-specific restoration is required to interpret
  subsequent messages.
- Choose numeric limits for entries, names, descriptions, instructions and
  catalog storage together with `docs/limits.md`. Account for UTF-8
  conversion, JSON escaping, history admission, MCP schemas and images, plus
  static copies and guest heap headroom. Do not assume the current instruction
  loader's truncation policy is suitable for skills.
- Choose a skill-package text reader and frontmatter parser that accept the
  standard format and external text encodings without silently treating UTF-8
  bytes as MacRoman. Define how in-situ authoring through the current text
  tools produces conforming packages, including handling of unsupported
  characters. Do not change all workspace text conventions implicitly.
- Standard package-relative reference paths must resolve to validated classic
  workspace paths. Preserve alias refusal and workspace containment; do not
  interpret references as host paths. Declare OS 9 path and resource limits
  separately from format validity, and report incompatible entries clearly.
- Support the specification's defined frontmatter fields and naming rules.
  Resolve name conflicts deterministically: the workspace authoring override
  is an intentional precedence rule, not an ambiguous duplicate. Other
  ambiguous names are unusable. Never select whichever file was scanned first
  or replace a user's authoring copy when updating the bundled default.
- Runtime documentation must explain installation, selection, invalid entries,
  conversation behavior, authoring and compatibility limits when implemented.
  Host fault tests and an OS 9.2.2 diagnostic must verify the relevant loader,
  UI and persistence behavior before claiming support.
- Review assumptions: the leading `@` syntax, library folder name,
  `write-skill` name and chooser insertion behavior are initial design details
  proposed here. Parser implementation, budgets, bundled delivery and external
  text handling remain open. No decider handle was supplied; record it when
  accepting this ADR.
- Revisit when users need combined or automatic selection, larger instructions,
  a shared global library or other library scopes, import tooling, executable
  packages or automated skill evaluation, or when measured guest/request costs
  make instruction delivery impractical.

This complements ADR-0003 and ADR-0004 and supersedes neither. It records the
target design; skills are not implemented by adding this document.

## References

- [ADR-0003: Admit platform tools read-only first and journal every mutation](0003-admit-tools-read-only-first.md)
- [ADR-0004: Load AGENTS.md as bounded owner guidance](0004-load-agents-md-as-bounded-owner-guidance.md)
- [Tool contracts](../tools.md), [project guidance and handoff usage](../usage.md),
  and [request and memory limits](../limits.md).
- [Agent Skills specification](https://agentskills.io/specification) (consulted
  2026-10-12): package structure, frontmatter, resources and validation.
- [Anthropic's skill creator](https://github.com/anthropics/skills/blob/main/skills/skill-creator/SKILL.md)
  (consulted 2026-10-12): baseline comparisons inform the usefulness principle;
  its automated evaluation machinery is not required here.
- Maintainer conversation on 2026-10-12: explicit activation first, installed
  skill discovery, and authoring good skills in situ.
