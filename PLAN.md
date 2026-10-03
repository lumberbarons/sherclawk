# Sherclawk first pass

The name is Sherclawk (Sherlock + claw); the mascot is a lobster.

Sherclawk is an independent source copy of HelloChat in this directory.
HelloChat remains unchanged. Build Sherclawk with Retro68; execute its agent
loop and tools in the classic Mac application.

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
   Search, guarded overwrite/edit, folder creation and automatic recovery remain
   unimplemented; read revisions are still observational.
3. Capture a reproducible PowerPC Toolbox template and exact installed
   MrC/PPCLink/Rez/SDK versions. Native MacRelix file-job worker: complete-file
   publication, rename claim, stage/log/completion records, uncertain outcomes.
4. `create_project`, `build_project`, and native `run_application`; associate
   outputs with successful builds and source revisions, and correlate runtime
   smoke-test results by run ID. Never launch an artifact from a failed build.
5. Counter application acceptance task, compiler-error repair, then a file
   transformer and drawing application. Validate logic automatically and
   appearance manually. Add stronger recovery and context compaction later.

## Boundaries and verification

The native app owns prompts, tool validation, sessions, and execution. Direct
OpenRouter HTTPS provides inference. MacRelix is only the later build executor.
No generic shell, desktop control, streaming, MCP, or subagents in this pass.

Run host protocol/transport checks under ASan/UBSan. Build and publish
fork-aware, then exercise a real tool-result-follow-up cycle and Stop in OS 9.
Check malformed/truncated calls, unknown tools, bounds, path escape, binary
reads, encoding, history capacity, and journal failures before advertising
mutating tools. Never retry uncertain mutations automatically.
