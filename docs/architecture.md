# Architecture and execution contracts

Sherclawk is a native C99 Toolbox application for classic Mac OS 9. The host
cross-compiles it with Retro68; the running Mac application owns prompts,
validation, persistence, tool execution and the cooperative event loop.
OpenRouter HTTPS supplies inference. [Current tool contracts](tools.md),
[native build behavior](native-builds.md) and [buffer limits](limits.md) are
specified separately. Active work lives in the
[issue tracker](https://github.com/lumberbarons/sherclawk/issues).

## Display boundary

`main.c` owns the Toolbox controls, TextEdit handles and drawing. `display.c`
contains pure token/tool formatting, model and prompt validation, pause reasons,
scroll calculations and layout rectangles. Its inputs and outputs use ordinary
C values; the UI adapts the rectangles to QuickDraw and applies scroll offsets.
The transcript and MCP editor share the same scroll calculations.

`chat_append_message()` in `chat.c` converts UTF-8 to bounded MacRoman/CR display
text, substitutes session-file notices for oversized entries, and caps accumulated
transcript bytes and separators. When the next entry does not fit, it drops the
oldest whole entries, behind one session-file notice, rather than clearing the
window; `Chat` records where each entry starts, so a reply with blank lines of its
own is never cut in the middle. It returns the entry offset for tool-line folding.
The UI then refreshes TextEdit and scrolls to the bottom. Display capping leaves
conversation history and the session journal intact. These helpers run in host
ASan/UBSan checks; the scroll diagnostic still exercises the real guest controls.

## Harness direction

The agent chooses application behavior, source organization and repair steps.
The harness supplies file operations, compilation, launch and observations,
with fork-aware files, revision-bound snapshots, journal barriers, bounded
execution, collision refusal and explicit uncertain outcomes. Do not encode a
fixed generated application in the harness.

The verified PowerPC Toolbox template is a starter and acceptance fixture.
`create_project` is optional: an independently assembled project must be equally
buildable when its descriptor is valid. Template identity and creation history
never authorize execution.

## Build/run contract

The supported [protocol-2 descriptor](native-builds.md#native-project-builds)
can name multiple C sources, Rez resources, headers and include paths, an output
name, and structured compiler/linker settings. Descriptor fields are data, never
shell or MPW command fragments. Validate schema, paths, settings and bounds
before publication, and reject unsupported inputs explicitly.

`build_project(path)` binds the descriptor, settings, recipe/toolchain identity
and all project-owned inputs to an immutable snapshot with revisions. It returns
a build ID and bounded diagnostics with log continuation. The verified backend
is MrC/PPCLink/Rez in the guest, executed through MPW ToolServer, the only
build backend. Queue locks are not liveness proof and are never stolen
automatically.

`run_application(build_id)` resolves the artifact and persisted authority for
that successful snapshot, verifies both forks and native identity, and reserves
a separate run ID before launch. Failed, partial and uncertain builds cannot
authorize launch. Editing source files never changes the identity of an older
build. A `process_present` observation is distinct from a functional smoke test
or visual acceptance. New, artifact-matched processes launched by Sherclawk gain
volatile quit authority only after `run_observed` is journaled. New Chat preserves
it; app exit loses it. `quit_application(run_id)` rechecks identity, journals
intent and submits a queued noninteractive Quit through the shared answer
dispatcher. Only observed process disappearance proves success; Stop ends
observation without cancellation. Applications are never cleaned up automatically.

New descriptors use protocol 2; protocol 1 requires explicit migration and is
rejected before queue publication. Hash revisions are observational FNV tokens,
not cryptographic attestations. AFP FlushVol does not establish power-loss
persistence or protect against server-side writes.

## Cooperative work and persistence

Runtime work advances from the event loop in bounded steps and honors Stop.
Execute tools sequentially, journal assistant responses and starts before
execution, and retain completed results when a run stops. A recording failure
prevents further execution. An abandoned published job or outstanding Apple
event can still complete: stopping observation does not prove cancellation.
Never retry an uncertain mutation, build or launch automatically.

Source tools use paths relative to the configured workspace, refuse traversal
and aliases, and preserve classic MacRoman/CR `TEXT` files. Model requests,
responses and journals use UTF-8. Argument JSON must not pass through lossy UI
conversion. Detailed mutation recovery rules belong in [tools](tools.md).

Workspace and project `AGENTS.md` text is owner guidance, not tool authority:
the system rules say it cannot override them or widen a tool, and it never
changes which tools may run. The workspace-root text lives outside history (a
static in `agent.c`, in the system message of each request) so it survives New
Chat and handoff without being summarized; a project's text is a history
message, so it follows the conversation. See
[ADR-0004](adr/0004-load-agents-md-as-bounded-owner-guidance.md).

## Deliberate costs

Some apparently wasteful work is a guarantee. Changing any of the following
changes a documented contract, not just a timing, so treat it as a design
decision rather than a tuning knob. Performance work must measure in the guest
and leave these alone unless the contract and its tests change with it.

- `FlushVol` after journal records, per log line (so a host can tail it) and
  per completed staged input. These are visibility and crash-evidence barriers,
  not power-loss durability.
- Closed-file write-then-verify-read cycles for staged inputs, and the second
  pre-read of every descriptor and input in `build_project` before publication.
  These are the exact-bytes and revision guarantees.
- `run_application` hashes both artifact forks at authorization and again at
  launch, because the artifact may change in between.
- The one-tick event cadence while a run is active, and small bounded result
  and read pages. A File Manager call cannot be interrupted, so page size sets
  Stop granularity. Model-visible page sizes are also a product decision: they
  change what the model sees per result and how many rounds a task takes.
- Open Transport is fully cycled around every model request, because a failed
  connection can wedge it for all later attempts. A live exchange is never
  torn down mid-connect: aborts and quit let a pending connect or handshake
  settle first (pumped without sending the request, bounded at 30 seconds and
  logged if forced), because an outstanding async connect can fault the
  classic OT stack. A connected or finished exchange closes at once. The yields around that cycle are
  tunable and soaked; see the comment on `CloseChatContext` in `main.c` and
  [development](development.md).

## Preferences implementation notes

The modal dialog uses classic Dialog Manager edit items and Control Manager
checkboxes. Saved files have Finder type `pref` and creator `ShCk`; missing or
malformed individual values use compiled defaults. The clear-text key and
unencrypted file are deliberate current limitations, described in
[usage](usage.md#workspace-and-preferences). Changing the workspace affects new
work; an already-open session retains its own journal location.

On the initial implementation, `FSMakeFSSpec` returning `fnfErr` for a missing
preferences file had to be treated as an ordinary first-run case. `ModalDialog`
reported checkbox clicks without toggling them, so the application updated the
control value and redrew it explicitly. Those guest findings remain useful when
changing the dialog.

The storage choice was an app-owned `pref` file rather than `TEXT` or
resources. Per-key compiled fallbacks, immediate workspace changes for new work
and a display-only debug toggle were intentional. Classic conventions place
Preferences in the Edit menu, separated from editing commands, with
**MCP Servers…** after it; Return
accepts and Escape or Command-. cancels. The model chooser uses Preferences as
the sole source. Remaining follow-ups are tracked as issues.

## MCP configuration editor notes

The editor is a modal dialog with its own `TextEdit` record and scroll bar
rather than a dialog edit item, because an edit item is limited to 255
characters. It shares the pane scroll helpers and the Cut/Copy/Paste handler
with the main window, and runs its own event loop like Preferences. The
text rules (`mcp_editor.c`) and persistence (`mcp_store.c`) have no Toolbox
dependency beyond the File Manager, so host tests exercise them against a
fault-injecting model. Saves stage, verify and swap rather than truncating in
place: the file is hand-edited and holds credentials, so a failed save must
never cost the previous version.

## Model catalog and context display

The context window comes from the public
`GET /api/v1/models?q=<model>&limit=10` catalog query: the row whose `id`
exactly matches the model supplies `context_length`, and its optional
`reasoning` metadata (`supported_efforts`, `default_effort`, `mandatory`,
`default_enabled`) is kept with it. Substring matches such as `-pro` or
`:batch` variants, dated aliases and other rows are ignored. The app requests
it at most once per model per launch, with a 30-second deadline and the same
64 KiB response bound. Any HTTP error, timeout, oversized body, a page with no
exact row, or a response beyond the 4096-token JSON parser cap silently omits
the percentage and proceeds with the send; there is no retry. Preferences are
unavailable during a run, including the context lookup. Stop during the
lookup leaves no session record and keeps the prompt; the stopped exchange
continues to a terminal network state before it is closed, so a send in the
next few seconds may briefly report that the lookup is still closing.

## Memory and history

Conversation buffers are static and allocated at launch. An additional history
byte also enlarges candidate history, JSON and HTTP request storage, so capacity
changes multiply RAM usage and increase each round's upload and input cost.
The TextEdit display has a separate cap. [Limits](limits.md#memory-cost-per-byte-of-cap)
contains the measurements, estimates and update requirements; keep that
reference current before changing memory or the `SIZE` partition.

## Visual identity and acceptance

Use `art/sherclawk.png` for Finder icons and the character in the main window.
Generate native resource data deterministically; retain conversion source and
artwork in Git, and generated resources in `build/`. Preserve usable native
controls and verify both Finder icon behavior and the main window in OS 9.

A generated application must preserve ordinary Toolbox event handling and
standard controls suitable for its behavior. The starter includes confirmed
close-box handling and Command-Q; updating the starter never changes already
created projects.

Host checks establish protocol and modeled fault behavior. Guest acceptance
must exercise real File Manager/ToolServer behavior, native process observation,
tool-result follow-up and Stop. Test independent and starter-created projects,
compiler-error repair, fresh rebuilds and the exact successful artifact.
Do not advertise planned capabilities as verified. Preserve dated evidence in
[verification history](history/verification.md) and the component records.
