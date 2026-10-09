# Historical Preferences notes

The initial Preferences dialog was guest-verified on October 7, 2026. This file
retains unresolved considerations from that design discussion. The original
proposal mixed pre-implementation behavior with later decisions, including an
in-window model override that has since been removed; it is retired.

Use [the usage guide](../usage.md#workspace-and-preferences) for current model,
key, workspace and run-limit behavior, and
[architecture](../architecture.md#preferences-implementation-notes) for the
remaining implementation observations. The general
[verification history](../history/verification.md) records the guest checks.

The storage choice was an app-owned `pref` file rather than `TEXT` or resources.
The key is displayed plainly and stored without encryption. Per-key compiled
fallbacks, immediate workspace changes for new work, and a display-only debug
toggle were intentional. The classic conventions place Preferences last in the
Edit menu, separated from editing commands; Return accepts, Escape or Command-.
cancels. The model chooser now uses Preferences as the sole source.

## Considerations from the original proposal

These were proposed follow-ups, not a current roadmap or acceptance claim.
Consult the [issue tracker](https://github.com/lumberbarons/sherclawk/issues)
before treating one as active work:

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

## Historical sources

The discussion consulted the Mac OS 8 Human Interface Guidelines and locally
installed Universal Interfaces: `Folders.h`, `Dialogs.h`, `Controls.h`,
`MacWindows.h`, `Resources.h`, `KeychainHI.h` and `RIncludes/Dialogs.r`.
Keychain was introduced with Mac OS 8.6; its presence in headers alone did not
establish a tested key-storage workflow. API availability and original source
line references were research evidence, not shipped feature acceptance.
