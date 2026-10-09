# 004 — Sherclawk tools for uniquely OS 9 subsystems

**Status:** step 1 built and guest-verified (October 2026): `get_file_info`,
`resolve_alias`, `list_processes`, `list_fonts`, `measure_text`,
`list_resources` and `read_resource` are implemented in
`inspect.c`, covered by the host model and accepted by
`SherclawkInspectCheck` plus a live six-tool probe conversation in OS 9.2.2
(see `README.md`). The journaled mutations, process-quit
integration and the remaining options below are still ideas. Presence of
optional extensions on the actual 9.2.2 install (Speech, Translation
Manager, OSA) still needs a guest check before any such tool is advertised
(October 2026).

## The idea

`run_application(build_id)` completes the "build an application" path. The tools
worth adding after it are the ones only classic Mac OS makes possible: the
Resource Manager, Finder identity, the cooperative Process Manager and Apple
Events, QuickDraw text metrics, OSA/AppleScript, Sound and PlainTalk, and HFS
catalog objects such as aliases and the Trash.

These are the tools a Unix-shaped harness never needs and a Mac-shaped one cannot
skip. They let the model not only write classic applications but also inspect
what it actually produced, make files behave correctly in the Finder, run and
quit its own artifacts, measure UI it cannot see, and manage a real OS 9 disk.
General-purpose gaps (diff, move/rename, copy) exist too, but they are not the
subject here — this list is deliberately the platform-differentiated one.

Inclusion rule, matching the existing harness:

- Read-only tools first; they are cheap to add safely and need no journal.
- A mutating tool must join the existing intent/staged/committed journal,
  staging, verification and explicit-uncertainty model. No silent retries.
- Built artifacts stay immutable evidence. Resource or metadata edits target
  workspace sources, never a recorded build output.
- Every result must fit `AGENT_RESULT_CAP` (1536 bytes, `agent.h`) and paginate
  with a `next_*` continuation where needed.

## What Sherclawk does today

- Ten advertised tools: `get_environment`, `list_files`, `read_text`,
  `search_text`, `write_text`, `create_folder`, `create_project`,
  `build_project`, `read_build_log`, `edit_text`. The schemas are one static
  string in `agent.c`; `main.c` intercepts the cooperative `build_project` and
  `read_build_log`, and `tools.c` dispatches the rest (main.c:983-988,
  tools.c:940-957).
- Paths are relative colon-separated MacRoman names under `SHERCLAWK_WORKSPACE`,
  validated in `tools_validate_path` and `spec_for` (tools.c:39-63). Aliases,
  resource-fork files and binary contents are refused by the text tools
  (`plain_file`, tools.c:161-175). `read_text` explicitly refuses nonempty
  resource forks (`ioFlRLgLen`).
- `full_revision` is dir ID + modification time + data-fork length + an FNV
  hash of the data-fork bytes (tools.c:176-189). `same_file` additionally
  compares fork lengths, type/creator and Finder flags (tools.c:190-201), so
  metadata changes already trip staleness checks inside mutations, but the
  revision token itself does not cover the resource fork or Finder info.
- `get_environment` reports the system version hex, architecture, workspace,
  encoding, free heap and policies (tools.c:101-114). It has no other Gestalt
  results and no disk free space.
- Per-run bounds: four calls per response, 32 executed calls, 16 rounds; each
  call's arguments at most 8 KiB and each result at most 1536 bytes
  (`agent.h:5-12`). `run_application` is planned and must resolve only an
  artifact recorded for a successful build, with its own run ID
  (`PLAN.md` build/run contract).

## 1. Resource tools — the biggest gap and the most OS 9 thing there is

`list_resources(path)` and `read_resource(path, type, id)`, read-only.

- A classic application *is* a resource collection. Today the model writes Rez
  and only sees compiler diagnostics; it cannot confirm that 'MENU', 'DITL',
  'DLOG', 'vers' or 'icns' actually made it into the artifact.
  `list_resources` beside `read_build_log` closes that loop and also lets the
  model study existing applications on the disk instead of guessing Rez syntax.
- Decode the common text-shaped types to readable text ('TEXT', 'STR ', 'STR#',
  'vers', 'MENU', 'DITL', 'DLOG', 'ALRT', 'FREF'); hex-dump everything else
  with the same bounded-page discipline as `read_text`. A resource map page
  should be capped like `list_files` (e.g. 64 entries plus a cursor).
- APIs are present in the vendored headers: `CountResources` (Resources.h:344),
  `GetIndResource` (:368), `Get1Resource` (:410), `GetResInfo` (:527),
  `GetResourceSizeOnDisk` (:574). Raw fork bytes can come from the Files.h fork
  calls (`FSOpenFork` etc., InterfaceLib 9.0+).
- This is the deliberate read-only exception to the current resource-fork
  refusal. If a `copy_resource`/`write_resource` ever earns its way in, it
  needs whole-fork staging and verification, whole-fork revisions, and must be
  blocked from touching recorded build artifacts; the agent should keep editing
  `.r` sources and rebuilding.

## 2. Finder identity — files that are what they say they are

`get_file_info(path)` and a journaled `set_file_info(path, ...)`, plus
`resolve_alias(path)`.

- Files identify themselves by type and creator, and the Finder decides
  double-click and icon behavior from those plus flags — not from extensions.
  Reading: `fdType`, `fdCreator`, `fdFlags` (`kHasCustomIcon` 0x0400,
  Finder.h:186; `kHasBundle` 0x2000, :189; `kIsAlias` 0x8000, :191), label
  bits, both fork lengths, dates and the lock bit (Finder.h:301, Files.h:1623).
- Writing should cover only catalog metadata: type/creator, label, lock,
  bundle/custom-icon flags. Never fork bytes. Journaled
  intent/committed with verification, exactly like the folder tool. Decide
  explicitly whether these fields join the revision token or get a
  metadata-only revision scope; `same_file` already treats them as staleness
  relevant.
- Custom icons are an Icon Utilities job: `SetIconFamilyData`/`GetIconFamilyData`
  and `AcquireIconRef` (Icons.h:1426, :1451, :1502), available since 8.5. The
  publication path already carries the app's own icon successfully; this makes
  workspace files and documents look right without a Desktop DB rebuild.
- `resolve_alias` is read-only and fills a real hole: aliases are first-class
  HFS objects that every current tool refuses, and real disks (Apple Menu, user
  folders) are full of them. `NewAlias`/`ResolveAlias` are in Aliases.h
  (:102, :160). Alias creation is optional and later.
- Why this is OS 9-specific: on no other platform does catalog metadata decide
  how a file opens and how it is drawn.

## 3. Process Manager and Apple Events — the era's app lifecycle

`list_processes` plus a polite `quit_application(run_id)`; extend
`run_application` to own the PSN it launched.

- Cooperative multitasking means the artifact, the MacRelix worker and the
  user coexist. "Still alive?" and "quit cleanly" are the only sensible smoke
  test primitives, and they need the Process Manager:
  `GetProcessInformation` (Processes.h:306), `SetFrontProcess` (:320),
  `LaunchApplication` (:238).
- Quit with an Apple Event: `kAEQuitApplication` (AppleEvents.h:90), with a
  reply/timeout. Scope event sending to PSNs the harness launched; never target
  the Finder or arbitrary user apps. This tree's Processes.h does not declare
  `KillProcess`, so any forced-quit fallback needs an explicit availability
  check before it becomes a tool.
- `list_processes` also surfaces the README's own worker gotcha: an idle
  background worker needing foreground activation is visible as a process, not
  as lock-file evidence. A later `worker_status` convenience could fold queue
  counts and liveness together.

## 4. QuickDraw metrics — the model's missing ruler

`list_fonts` and `measure_text(text, font, size, style)`.

- The agent cannot see the UI, and pixel-exact classic layout depends on the
  fonts actually installed in the guest. These are the closest thing to a
  preview available: `StringWidth` (QuickdrawText.h:564), `CharWidth` (:552),
  `TextWidth` (:576), `GetFontInfo`, and Font Manager family lookup.
- Fonts are installed `FOND`/`nfnt` resources and machine-specific; a dialog
  that fits Chicago 12 is not the same as the model's guess. Keep it small:
  a handful of strings per call, widths in pixels.
- Stretch once it exists: a `measure_dialog`-style check that reads a project's
  DITL and reports whether its items fit — still read-only, still bounded.

## 5. Gestalt capabilities and volumes

`get_capabilities` and `list_volumes`/`get_volume_info`.

- Feature detection is the OS 9 pattern. Report booleans/versions for QuickTime,
  Open Transport, Color QuickDraw, Sound, Speech, AppleScript support, RAM and
  processor, and which optional extensions are present. Keep it separate from
  `get_environment` so neither result grows past a comfortable slice of the
  1536-byte cap.
- Volumes are `FSGetVolumeInfo`/`PBGetVolumeInfoSync` (Files.h): desktop name,
  total and free bytes, read-only flag. Free-space preflight prevents
  confusing worker failures in a multi-file build; read-only explains whole
  classes of error.

## 6. HFS catalog semantics — Trash (aliases are in §2)

`move_to_trash(path)`, journaled.

- The era has no `rm`; `kTrashFolderType` (Folders.h:76) plus a same-volume
  rename is the period-correct, reversible delete. Finder semantics on shared
  volumes differ (an AFP network trash), so fall back to a workspace archive
  folder only if the volume has no trash. Verify, journal, never auto-retry —
  the user can still recover the file.
- This is the safe way to clean up failed fixtures and superseded outputs
  without introducing a raw delete primitive.

## 7. OSA — compile scripts before deciding whether to run them

`check_script(source)`, compile-only.

- OSA.h declares the compile/store/execute family (`OSACompile`, `OSALoad`,
  `OSAStore`, `errOSAScriptError` among its mode flags and error codes). A
  compile-only tool reports AppleScript syntax errors and line numbers, executes
  nothing, and is the automation equivalent of `build_project`: the model can
  produce OS 9 automation deliverables safely.
- Full `run_script` is the era's shell and collides with `PLAN.md`'s "no
  generic shell, desktop control, streaming, MCP, or subagents" boundary
  (PLAN.md:110-120). If ever pursued, scope it, default it off, and require
  explicit confirmation for mutating verbs.
- The same Apple Events facility from §3 makes launched artifacts scriptable;
  treat that as the intended integration rather than a general scripting tool.

## 8. Sound and PlainTalk — era-correct notification

`speak_text(text)` and `play_sound(path or snd id)`.

- Long builds and smoke tests take minutes while the user is at the machine.
  PlainTalk is genuinely unique to this platform and is the correct notification
  channel of the era: `SpeakString` (SpeechSynthesis.h:720), `SpeakText` (:732);
  `SndPlay` (Sound.h:1558) for 'snd ' resources.
- Gate on Gestalt; link the additional Speech/Sound libraries as needed; keep
  the result tiny. Fun, but also actually useful.

## 9. Transfer formats — getting artifacts out of 1999

`export_macbinary(path, destination)` and optionally `binhex(path)`.

- MacBinary III is a 128-byte header plus both forks; BinHex 4.0 is the
  email-era encoding. This is how built applications travelled between
  machines, and how the emulator world still trades them. The workspace already
  deals in fork-aware transport (`tools/deploy-to-share.sh`,
  `tools/netatalk_meta.py`), and AGENTS.md's rule about MacBinary/sidecars
  applies in reverse when exporting.
- Guest-side File Manager can read both forks; the encoding is a pure function
  of the bytes. A natural companion to manual launch: build, verify, export,
  hand the user a `.bin`.

## 10. Footnote-grade

- Translation Manager `TranslateFile` (Translation.h:275) converts between
  system-known formats (TEXT, PICT, ...) when the extension is installed. Very
  OS 9, moderate utility; guest-verify extension presence first.
- A screenshot/`capture_window` tool would need an image channel to the model,
  which the current text protocol does not have — an artifact for the user, not
  a model observation, until multimodal results exist.
- Edition Manager publish/subscribe is as OS 9 as it gets and probably not
  worth the cost.

## Recommended sequence

1. Read-only first, in this order: `list_resources`/`read_resource`,
   `get_file_info`, `list_processes`, `measure_text`/`list_fonts`. All fit the
   result cap, need no journal, and immediately strengthen the
   build → verify → run loop. **Built and guest-verified (October 2026)**,
   including `resolve_alias` in the same read-only batch.
2. Journaled catalog mutations: `set_file_info`, `resolve_alias` (read-only),
   `move_to_trash`.
3. Integrate into `run_application`: PSN ownership, polite `kAEQuitApplication`,
   liveness reporting; then `list_processes` earns its keep in diagnostics.
4. `get_capabilities` and volume info.
5. Delight: `speak_text`/`play_sound`.
6. `check_script`; make the `run_script` boundary decision explicitly.
7. `export_macbinary`.

## Verification, when built

- Host: extend `tests/toolbox/`'s File Manager model and
  `tools/check.sh` ASan/UBSan coverage for any catalog mutation; resource
  listing/paging needs fixture files with known resource maps; Finder-info
  read/write needs round-trip and fault-injection tests like `write-check.c`
  and `edit-check.c`.
- Guest: new fixed diagnostics in the established pattern — a resource-list
  check, a Finder-info round-trip check, a process launch/quit check — plus a
  live main-app conversation through the model. Optional-extension tools need a
  Gestalt preflight and a skip path when absent.
- Per AGENTS.md, host success does not prove guest behavior; every advertised
  tool needs a real 9.2.2 run before it appears in `agent_tool_schemas`.

## Open questions

- Should resource reading cover arbitrary paths on the disk, or workspace plus
  recorded build artifacts only? System files have very large resource maps;
  the answer decides the map paging bound and the path policy.
- Does `set_file_info` extend the revision token or get a separate metadata
  revision scope? `same_file` already compares the fields, so only the token
  format is undecided.
- Do custom icons written guest-side survive netatalk/AFP publication, or is
  the existing host-side publication path (bundle + custom icon flags) still
  required for shared volumes?
- What should `run_application` do for a non-Apple-Events-aware artifact —
  timeout then report, or a user-confirmed forced quit after an availability
  check?
- Does `OSACompile` on a script with `using terms from` blocks trigger app
  resolution dialogs? Guest test needed before `check_script` is safe.
- Is Speech/Translation Manager/OSA actually installed on this guest, and
  should absence be an error or a capability flag?
- Grow `get_environment` or keep `get_capabilities` separate? Separate seems
  right for the result cap.
- MacBinary or BinHex as the default export, and do exported files live beside
  the build artifact or in a dedicated workspace folder?

## Evidence

- Locally installed, user-supplied Universal Interfaces, `InterfacesAndLibraries/Interfaces/CIncludes/`:
  - `Resources.h` — `CountResources` :344, `GetIndResource` :368,
    `Get1Resource` :410, `GetResInfo` :527, `GetResourceSizeOnDisk` :574.
  - `Finder.h` — `kHasCustomIcon` :186, `kHasBundle` :189, `kIsAlias` :191,
    `fdFlags` :301.
  - `Aliases.h` — `NewAlias` :102, `ResolveAlias` :160, `ResolveAliasFile` :235.
  - `AppleEvents.h` — `kAEQuitApplication` :90.
  - `Processes.h` — `LaunchApplication` :238, `GetProcessInformation` :306,
    `SetFrontProcess` :320; no `KillProcess` declared in this tree.
  - `QuickdrawText.h` — `CharWidth` :552, `StringWidth` :564, `TextWidth` :576.
  - `SpeechSynthesis.h` — `SpeakString` :720, `SpeakText` :732.
  - `Sound.h` — `SndPlay` :1558.
  - `Icons.h` — `SetIconFamilyData` :1426, `GetIconFamilyData` :1451,
    `AcquireIconRef` :1502.
  - `OSA.h` — `OSACompile`/`OSAStore`/`OSADoScript` mode flags and error codes
    around :442-523 and :678.
  - `Translation.h` — `TranslateFile` :275.
  - `Folders.h` — `kTrashFolderType` :76.
  - `Files.h` — `PBGetFInfoSync` :1623; `FSGetVolumeInfo`/fork calls noted as
    InterfaceLib 9.0+.
- `agent.h:5-12` (bounds), `agent.c:8-62` (static policy and
  schema string), `tools.c:39-63` (path policy), `:101-114`
  (environment), `:161-201` (plain-file refusal, revision, same-file),
  `:940-957` (dispatch), `main.c:983-988` (build dispatch).
- `PLAN.md:110-120` (boundaries: no generic shell or desktop
  control), `PLAN.md` build/run contract (`run_application` authorization).
- `README.md` sections "Native project builds" and "Controls, limits and
  sessions" for the current contract.
