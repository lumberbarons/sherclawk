# 002 — Sherclawk Preferences

**Status:** built and guest-verified October 7, 2026 in `skimmer` on OS 9.2.2:
dialog, save/relaunch persistence, validation refusals, configured limit
pauses, the debug view and an immediate workspace change all passed. A build
without compiled credentials should still prove the saved key end to end.

## The idea

A native Preferences dialog to set the **model**, **API key**, **workspace**
and **per-run limits** — the model rounds and executed tool calls a single run
may use — inside the running app, saved to a file in the guest's System
Folder. Builds no longer need credentials baked into `config.local.h`, and the
workspace and run limits can change without a rebuild. The dialog also carries
one display option, **Show
tool debug in Conversation** (default off), which adds each tool call's debug
record — call ID, raw arguments and journal events — beneath its result in the
Conversation.

## What Sherclawk does today

- `config.local.h` (gitignored) supplies `SHERCLAWK_MODEL`, `SHERCLAWK_API_KEY`
  and `SHERCLAWK_WORKSPACE` at compile time; `config.h` provides defaults, and
  a UI build without credentials still launches.
- The model is already editable per run in the main window (`gModelTE`). The
  API key and workspace are not; `SendChat` refuses to send without a
  compiled-in key and tells the user to set `config.local.h` and rebuild.
- The per-run ceilings are compile-time constants in `agent.h`:
  `AGENT_TURN_MAX` 32 and `AGENT_TOOL_MAX` 64. `main.c` enforces both in the
  loop — `StartModelRequest` refuses another model round and `DriveChatStep`
  refuses the next tool call once the matching counter reaches its ceiling —
  and `PauseRunAtLimit` reports the counts and history percentage and reminds
  the user to send Continue. `agent_begin` resets both counters on the next
  message, so the ceilings are per run, not per session. `agent.c` never reads
  the constants; only `main.c` compares them.
- The Conversation shows each tool result under the tool name
  (`ShowMessage(call->name, gToolResult)`); a call's ID, arguments and journal
  events are invisible there. `agent_response` already records the whole
  assistant message (arguments included) into the session JSONL before
  execution, and `Sherclawk.log` carries lifecycle lines only.
- The Edit menu has items 1–8 (Undo … Select All), all selected by item number
  in `HandleMenu`, so a Preferences item appended after a separator becomes
  item 10 and disturbs nothing (but it must be handled outside the current
  focus guard).

## The OS 9 conventions

- The word is **Preferences**, not Settings. The Mac OS 8 HIG standardized the
  Preferences command as the last item of the **Edit menu**, preceded by a
  separator and written with an ellipsis (`Preferences…`); "Settings" is a
  macOS 13+ rename.
- The screen is an ordinary modal Dialog Manager dialog: `'DLOG'` and `'DITL'`
  resources compiled by Rez into `hello.r` (add `#include "Dialogs.r"`), one
  `EditText` item (DITL type 16) per setting plus labels and OK/Cancel.
  `GetNewDialog`, `ModalDialog`, `SelectDialogItemText` and
  `SetDialogDefaultItem`/`SetDialogCancelItem` handle focus, Tab between
  fields, Return = default and Command-. = cancel; `InitDialogs` is already
  called at startup. The debug toggle is one `CheckBox` item (DITL type 5),
  which the Dialog Manager toggles on its own; the save path reads its state
  with `GetControlValue` and seeds it with `SetControlValue`.
- Per-field cap: the Dialog Manager's text calls take Str255, so each field
  round-trips at most 255 bytes. Fine for OpenRouter keys (~73 chars) and
  model IDs. Validate the model and workspace the way `SendChat` already
  validates the model.
- The backing file lives in **System Folder:Preferences**, found with
  `FindFolder(kOnSystemDisk, kPreferencesFolderType, kCreateFolder, …)` and
  then `FSMakeFSSpec` + `FSpCreate`/`FSpOpenDF` (the calls `tools.c` already
  uses). Name it `Sherclawk Preferences`, creator `ShCk` (already the app's
  creator in `CMakeLists.txt`), type `'pref'` or `'TEXT'`. `FindFolder`
  follows Multiple Users redirection automatically, and because the guest
  writes the file through the File Manager, none of the netatalk fork-aware
  publication machinery applies.

## Storage format

`key=value` lines in MacRoman with CR line endings, read whole-file (it is
tiny):

```
model=openai/gpt-6-luna
api_key=sk-or-v1-...
workspace=Retro68:
max_rounds=32
max_tools=64
show_tool_debug=0
```

Tolerate blanks and comments, ignore unknown keys, and fall back to
`config.local.h` defaults for any missing or malformed value instead of
failing. The two run limits are decimal integers, 1–128 each; missing, blank,
non-numeric or out-of-range values mean the built-in default (currently 32
model rounds and 64 tool calls). The debug toggle is a boolean (`0`/`1`);
missing, blank or unrecognized values mean the built-in default, off. Rejected alternatives: a
`'pref'` resource (period-authentic but opaque and more code), JSON via the
existing `json.c` (consistent but anachronistic and creates a UTF-8/MacRoman
mix), and Internet Config
(system-wide internet settings, wrong fit for app credentials).

## Runtime behavior

- Load at startup before the window is built. Precedence: Preferences file →
  `config.local.h` → built-in defaults. The file is created on first save, so
  existing builds are unaffected until the dialog is used.
- Save on OK only; Cancel discards. Seed the dialog from the active values so
  a first run shows what is compiled in. Update `gModelTE` after a save, and
  change `SendChat`'s check from `SHERCLAWK_API_KEY[0]` to the runtime value.
- The workspace is currently a macro read in `tools.c` (`spec_for`,
  `get_environment`) and `main.c` (session folder, status text), so runtime
  configuration means threading one value through the tool layer (a small
  global set from prefs, or a context parameter). Validate it with the same
  rules the tools apply to relative paths (MacRoman, non-empty, ends in a
  colon, no leading colon, parent traversal or slash), then apply it to new
  sessions; a session already open keeps writing to the folder it was opened
  in.
- The run ceilings load with the same file and fallback rules (missing or
  malformed values use the built-in defaults). Keep `AGENT_TURN_MAX` and
  `AGENT_TOOL_MAX` as the compiled-in defaults and give `main.c` two runtime
  values seeded from Preferences; replace the two comparisons in
  `StartModelRequest` and `DriveChatStep` with them (`agent.c` needs no
  change). The dialog validates each field as decimal 1–128 on OK and refuses
  to save otherwise; a stored value that is not a number or is outside that
  range is ignored in favour of the default. The pause message still reports
  the actual counts and names the configured ceilings, so a paused run
  explains itself. The ceilings stay per run — the next message still resets
  the counters — and neither the 256 KiB history cap nor the 120-second
  request deadline becomes configurable; history remains the practical
  ceiling. A saved change applies to the next round or tool decision, and the
  `gSending` gate already prevents editing during a run.
- The debug toggle affects display only. With it on, each executed call's
  block follows its result in the Conversation: call ID, the arguments string
  the tool layer already parses (`AgentCall.arguments`, also preserved in the
  session's `assistant` record) and the journal event names emitted for the
  call (`tool_started`, plus `mutation_intent`/`staged`/`backed_up`/`committed`
  where they occurred). This is the in-window counterpart of reading the
  session JSONL from the share. Emit it at the existing
  `ShowMessage(call->name, gToolResult)` site, so user, model and notice
  messages never change, and truncate the arguments so a block cannot evict
  the transcript. Default off: the Conversation stays clean, arguments and
  results can contain workspace source, and the API key travels only in the
  HTTP request header — never in prompts, arguments or results — so no debug
  view can echo it.
- Gate the dialog while `gSending`: `ModalDialog` blocks the cooperative
  `DriveChatStep` loop, so an in-flight HTTPS run would stall.
- Never echo the key in status text or the session journal (the README already
  promises logs omit credentials).

## Security

The prefs file is cleartext on a single-user OS either way, and it sits in the
System Folder, outside the tool sandbox — the model cannot rewrite its own
credentials. Moving the key out of the binary is still an improvement:
distributed builds no longer embed credentials, while `config.local.h` stays
as a fallback for local builds. If hardening is wanted later, the Keychain
Manager is available on 8.6+ (`KCAddGenericPassword`/`KCFindGenericPassword`;
Retro68 ships `libKeychainLib.a`), at the cost of lock prompts and complexity.

## Verification, when built

Host: parser checks (missing file, blank/short/garbage values, over-long
values, unknown keys, CR/LF, boolean and limit values — empty, `0`, negative,
non-numeric, `129`, surrounding spaces) under the existing ASan/UBSan
checks. Guest:
open Edit → Preferences, save, inspect
`System Folder:Preferences:Sherclawk Preferences` in SimpleText/ResEdit,
relaunch and confirm persistence, change the workspace and create a file
through a real run, then send a request using the saved key. Set the limits
low (for example 2 rounds and 1 tool), run a request that needs more, and
confirm the run pauses at the configured counts and continues on the next
message; then restore the defaults. Toggle Show tool
debug, run one tool call with it off and on, and confirm the difference in the
Conversation; the session JSONL should be identical either way, and the toggle
should survive a relaunch.

## Decisions taken when built

- **File type is `'pref'`** (creator `ShCk`, app-owned). SimpleText hand-editing
  was the rejected alternative; ResEdit can still inspect it.
- **The in-window model field stays** as a per-run override. Preferences supplies
  the persistent default; saving the dialog also seeds the model field.
- **The key is shown plainly.** The classic Dialog Manager has no password item;
  obscuring would mean a custom `userItem`.
- **Limits are 1–128 including raising** above the compiled 32/64, with a hard
  validation stop on OK; malformed stored values fall back to the compiled
  default per key. The pause message names the configured ceilings.
- **The workspace applies immediately** to tools, inspection and native builds;
  a session already open keeps writing to the folder it was opened in. The
  session, handoff and build-queue paths are all derived from the runtime value
  (`tools_workspace()`), so a change strands nothing mid-session.
- **Debug blocks follow an agent-tool display style:** every executed call
  shows a call header with rendered arguments (`• write_text(path: "x")`); the
  toggle only adds the result indented under `»` and the call's journal event
  names. Staging/backup paths and full payloads stay in the session JSONL,
  which is byte-identical either way.
- **Guest-found details:** `FSMakeFSSpec` returns `fnfErr` for the missing
  preferences file, so first run and first save treat that as "no file yet";
  and `ModalDialog` reports a DITL checkbox click as its item number but does
  not toggle it, so the dialog tracks the toggle itself and redraws with
  `SetControlValue` + `Draw1Control`.

## Later / follow-ups

Good to have, not built:

- **Keychain storage for the API key** (`KCAddGenericPassword` /
  `KCFindGenericPassword`, shipped on 8.6+) with Preferences as the fallback;
  pair with an obscured key field once there is a real lock-prompt story.
- **Workspace preflight:** resolve the typed workspace in the dialog and refuse
  to save a volume/folder that does not exist, instead of letting the first
  session or tool report it.
- **Workspace migration:** an explicit action to move or copy
  `Sherclawk Sessions:`, handoffs and the `Worker01:buildjobs` queue when the
  workspace changes, rather than only applying to new work.
- **Revert/reset:** a "Use Compiled Defaults" button, and an automatic reset
  prompt when the stored file is malformed rather than silently falling back.
- **Per-field help** (an explanation line for the selected field) and
  focus/Tab polish verified in the guest.
- **Debug view expansion:** optional staging/backup paths, per-tool
  human-readable result summaries (Claude-Code-style) instead of raw JSON,
  TextEdit styling for the call/result lines, and a copy-block-to-scrap action.
- **Profiles:** multiple named preference sets (for example per model/provider)
  with a chooser in the dialog, if one global set proves limiting.
- **Queue-mode hooks (idea 005):** once the queue service exists, the same file
  can carry `queue_path` and `start_queue` so Preferences can configure and
  start the build-queue role — the parser already tolerates new keys, but they
  should wait for the queue engine and only be written once implemented.
- **Export/import** of the preferences file, and reconsider a `'TEXT'`
  prefs type only if hand-editing from SimpleText turns out to be wanted.
- Deliberately out of scope, revisit only with new evidence: making history
  capacity, request deadline or streaming configurable.
- **Saved-key proof to record:** deploy a build without compiled credentials
  and confirm Preferences alone supplies a working key, then optionally add the
  empty-key override test (clear the field, confirm Send refuses).

## Open questions (resolved when built)

The questions below were the discussion points before implementation; the
decisions taken are recorded above.

- File type `'pref'` (app-owned) or `'TEXT'` (double-click opens SimpleText
  for hand-editing)?
- Keep the in-window model field as a per-run override, or make Preferences
  the sole source? Keeping it seems right; the dialog supplies the persistent
  default.
- Obscure the key field, or show it plainly like the era would have?
- Does a workspace change apply immediately to new sessions, or require
  New Chat / relaunch outright?
- Debug block scope: event names only, or also the staging and backup paths?
  The session file keeps the full records either way.
- Should Preferences be able to raise the run limits above the compiled 32/64,
  or only tighten them? Raising multiplies the worst-case cost of one Send,
  which is what the ceilings exist to bound; the proposed 1–128 range allows a
  raise with a hard validation stop.

## Evidence

- Mac OS 8 Human Interface Guidelines: *"The position of the Preferences
  command has been standardized to the bottom of the Edit menu"* and
  *"always the last item in the Edit menu … preceded by a separator line"*
  (interface.free.fr archive of Apple's PDF).
- `InterfacesAndLibraries/Interfaces/CIncludes/`: `Folders.h`
  (`kPreferencesFolderType`, `FindFolder`), `Dialogs.h` (`ModalDialog`,
  `editText` = 16, `GetDialogItemText`/`SetDialogItemText`), `Controls.h`
  (`GetControlValue`/`SetControlValue` for the checkbox), `MacWindows.h`
  (`movableDBoxProc`), `Resources.h` (`FSpCreateResFile`); `RIncludes/Dialogs.r`
  defines the DLOG/DITL templates and the CheckBox item type (5).
- Source tree: current macro uses, Edit menu item numbering,
  `HandleMenu`, `SendChat`, `agent.h` (`AGENT_TURN_MAX` 32, `AGENT_TOOL_MAX`
  64), the `main.c` limit checks and `PauseRunAtLimit`, `agent_begin`'s
  counter reset, and `tools.c` path handling, as described above.
- Keychain shipped with Mac OS 8.6 and is declared as KeychainLib 1.0+ in
  `KeychainHI.h`; `Retro68/ImportLibraries/libKeychainLib.a` is present.
