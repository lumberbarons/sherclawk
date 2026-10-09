# Architecture and execution contracts

Sherclawk is a native C99 Toolbox application for classic Mac OS 9. The host
cross-compiles it with Retro68; the running Mac application owns prompts,
validation, persistence, tool execution and the cooperative event loop.
OpenRouter HTTPS supplies inference. [Current tool contracts](tools.md),
[native build behavior](native-builds.md) and [buffer limits](limits.md) are
specified separately. Active work lives in the
[issue tracker](https://github.com/lumberbarons/sherclawk/issues).

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
or visual acceptance.

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
  connection can wedge it for all later attempts. The yields around that cycle
  are tunable and soaked; see the comment on `CloseChatContext` in `main.c` and
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
Preferences last in the Edit menu, separated from editing commands; Return
accepts and Escape or Command-. cancels. The model chooser uses Preferences as
the sole source. Remaining follow-ups are tracked as issues.

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
