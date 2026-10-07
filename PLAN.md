# Sherclawk first pass

The name is Sherclawk (Sherlock + claw); the mascot is a lobster.

Sherclawk is an independent source copy of HelloChat in this directory.
HelloChat remains unchanged. Build Sherclawk with Retro68; execute its agent
loop and tools in the classic Mac application.

## Harness direction

Build a flexible native coding harness. The agent chooses application behavior,
source organization and repair steps; tools provide file operations, compilation,
launch and observations. Keep correctness constraints in the harness: fork-aware
files, revision-bound snapshots, journal barriers, bounded cooperative execution,
collision refusal and explicit uncertain outcomes. Project shape and application
behavior are agent decisions within the supported toolchain and explicit limits.

The verified PowerPC Toolbox template is a starter and acceptance fixture.
`create_project` remains a convenience; using it is optional. A project assembled
with ordinary folder/text tools must be equally buildable when its descriptor
is valid. Neither template identity nor creation history authorizes execution.

## Build/run contract

Protocol 2 and `build_project` are implemented; see the supported descriptor in
[README.md](README.md#native-project-builds). Extend its explicit limits/settings
as additional toolchain behavior is verified:

- Describe multiple C sources, Rez resources, project headers/include paths,
  an output name and supported compiler/linker settings, including library
  selection. Paths remain relative to the project within the workspace.
- Let the agent edit the descriptor through ordinary text tools. Validate its
  schema, paths, settings and explicit size/count limits before publication.
  Treat template identity as provenance/defaults, not a fixed source layout.
- Start with the guest-verified MrC/PPCLink/Rez toolchain. Translate structured
  settings into a trusted executor recipe; descriptor fields are data, never
  shell or MPW command fragments. Toolchain adapters can expand support later.

`build_project(path)` accepts any valid project descriptor, including one
written by the agent without `create_project`. Bind the validated descriptor,
settings, recipe/toolchain identity and all project-owned build inputs to an
immutable snapshot with source revisions. Report unsupported settings or missing
inputs explicitly. Return bounded compiler diagnostics and a build ID, preserving
log continuation and uncertain outcomes from the native job protocol.

`run_application(build_id)` resolves only an artifact recorded for that
successful build and its snapshot. It assigns a separate run ID for a native process
observation. Functional smoke-test results remain separate. A failed, uncertain or partial build cannot
authorize launch. Later source edits do not turn an older artifact into a build
of those edits; report the snapshot associated with the launched build.

New `create_project` descriptors use protocol 2. Older protocol-1 descriptors
require explicit migration; they are rejected before queue publication.
Do not advertise planned build/run capabilities until implemented and verified.

First acceptance: build an application with two C sources and an agent-written
project descriptor using ordinary file tools, without `create_project`. Also
build a starter-created project with an added source and a changed output name.
For both routes, exercise compiler-error repair, a fresh revision-bound rebuild,
launch of the successful artifact and correlated runtime observation in OS 9.
This establishes flexibility before adding more application-specific fixtures.

## Required visual identity

- Use the workspace's `../sherclawk.png` (the requested Sherclawk reference)
  for a real Finder application icon, including small and large sizes,
  transparency masks, and monochrome fallback.
- Show the same character permanently in the main window beside the
  conversation. Keep the model, action status, message, Send, and Stop usable.
- Convert existing artwork deterministically into native resource data;
  retain only conversion source in git. Generated resources live in `build/`.
- Verify the Finder icon and main-window art in the actual OS 9 guest.

## Implementation sequence

1. Independent application target, icon and artwork, native model/tool/result
   loop, sequential dispatch, environment/list/read tools, persisted session
   records, action status, and Stop. This is the initial implementation slice.
2. Workspace search and source mutation: explicit MacRoman/CR/TEXT policy,
   revision guards, create-only writes, unique exact edits, recovery records.
   Create-only `write_text` is implemented with strict MacRoman/CR/TEXT,
   verified sibling staging, journal barriers and collision-safe publication.
   `edit_text` adds one exact unique replacement, whole-file revisions for
   files up to 4 KiB, exclusive opens, verified staging and retained original
   backups with journaled publication. Bounded literal workspace search is implemented with optional recursion,
   catalog/byte continuation and MacRoman/CR line numbers. Create-only `create_folder` (one level, journaled intent and completion) is
   implemented. General overwrite and automatic recovery remain unimplemented; larger-file read revisions are
   still observational.
3. Capture a reproducible PowerPC Toolbox template and exact installed
   MrC/PPCLink/Rez/SDK versions. The `templates/ppc-toolbox/` fixture is now
   guest-verified for compiler failure, fresh rebuild, native launch and
   actual ToolServer process identity. The MacRelix file-job worker now
   implements complete-file publication, rename claim, stage/log/completion
   records and uncertain outcomes. Native File Manager production and bounded
   cooperative polling are implemented in `jobs.c`, with an event-driven
   `SherclawkJobCheck` diagnostic. Trusted recipe inputs are closed and verified
   before ready publication; Stop/deadline never cancel or replay a published
   job. The Python producer remains diagnostic only. Source revision binding
   and model-facing build/run integration belong to step 4.
4. Template-backed `create_project` is implemented: verified C/Rez source and a
   versioned descriptor published by one journaled, create-only folder rename.
   `build_project(path)` implements flexible protocol-2 descriptors and native
   compilation. Native `run_application(build_id)` is implemented with persisted fork
   fingerprints, cooperative verification, journaled run IDs and exact artifact
   Process Manager observations. Verify both
   starter-created and independently assembled projects with multiple sources,
   configurable outputs and revision-bound artifact tracking. Functional smoke testing remains next.
5. Counter application acceptance task, compiler-error repair, then a file
   transformer and drawing application. Validate logic automatically and
   appearance manually. Add stronger recovery and context compaction later.

## Boundaries and verification

The native app owns prompts, tool validation, sessions, and execution. Direct
OpenRouter HTTPS provides inference. Native ToolServer self-builds implement idea
005 increment 3; MacRelix remains the exclusive-owner build fallback. Queue
creation, generalized queue service and convenience features remain later work.
No generic shell, desktop control, streaming, MCP, or subagents in this pass.

Run host protocol/transport checks under ASan/UBSan. Build and publish
fork-aware, then exercise a real tool-result-follow-up cycle and Stop in OS 9.
Check malformed/truncated calls, unknown tools, bounds, path escape, binary
reads, encoding, history capacity, and journal failures before advertising
mutating tools. Never retry uncertain mutations automatically.
