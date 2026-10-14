---
id: ADR-0007
title: Keep Sherclawk data separate from the installed application
status: proposed
date: 2026-10-13
deciders: []
supersedes: []
superseded-by: []
related: [ADR-0001, ADR-0003, ADR-0004, ADR-0005]
tags: [storage, workspace, installation, journal, diagnostics]
---

# ADR-0007: Keep Sherclawk data separate from the installed application

## Context and Problem Statement

Sherclawk currently defaults to the AFP volume `Retro68:` as its workspace,
stores journals and handoffs in that workspace's `Sherclawk Sessions:` folder,
and recreates `Retro68:Sherclawk.log` on launch. Preferences and MCP credentials
already live in the Preferences folder found through the Folder Manager.
A standalone installation needs persistent data independent of the application
file and the deployment share, with one or more workspaces and a discoverable
home for conversation and operational records.

## Decision Drivers

- The application must be installable by Finder copy into Applications and
  replaceable without replacing projects, journals or diagnostic evidence.
- Local operation must not require a volume named `Retro68`, an AFP server,
  a host executor or an installer merely to establish data folders.
- Several workspaces must coexist under one visible data folder. Selecting one
  must not expose its siblings or private application records to file tools.
- Classic Mac OS uses colon paths and catalog identities, not Unix home or
  Library directories. Folder Manager discovery must handle localized names
  and OS 9 Multiple Users redirection rather than hard-coded disk names.
- Journals are mandatory execution evidence; diagnostic collection is
  best-effort. Their failure behavior must remain distinct (ADR-0003/0005).
- Existing network workspaces, open records and outstanding native builds must
  not be silently moved, rebound or replayed during adoption of a local layout.
- Apple's OS 9 guidance places preferences in Preferences and recommends
  user-selectable Documents locations for personal documents and databases.
  Application Support primarily holds installed supporting code and data.

## Considered Options

1. **Separate application and user-selectable data folder** — install in
   Applications; default persistent data to `Documents:Sherclawk Data:`.
2. **Application and mutable data in one installation folder** — copy the
   binary, Workspaces, Sessions and Logs together.
3. **Mutable data in System Folder:Application Support** — place workspace
   content and application records beneath a Sherclawk support folder.
4. **Keep all records inside each workspace** — retain the current session
   layout and put diagnostics alongside it, with no central data root.

## Decision Outcome

Chosen option: **Separate application and user-selectable data folder**.

Distribute a Sherclawk application folder containing the native application,
readme and any application-private installed support files. The normal install
is a Finder copy into Applications. First launch offers to create
`Sherclawk Data` in the Documents folder or choose an existing data folder;
it creates a Default workspace for a new data folder. These operations do not
require a dedicated installer. MPW/ToolServer setup remains a separate build
prerequisite under ADR-0001.

The persistent layout is:

```text
Sherclawk Data:
    Workspaces:
        Default:
        Another Workspace:
    Sessions:
        <workspace ID>:
    MCP Results:
        <workspace ID>:
    Logs:
        Sherclawk.log
        <retained diagnostic records>
    Reports:
```

`Workspaces` groups the default workspace folders; its siblings contain
application-owned records. Sessions includes journals and saved handoffs.
Workspace IDs are stable, bounded identifiers independent of displayed names
and paths. Logs contains the main lifecycle log and bounded retained launch
evidence governed by ADR-0005; the main log cannot be the sole evidence if it
is replaced on launch. Reports is the default destination for explicit local
diagnostic exports.

The data root and active workspace root are separate identities. Ordinary
model file tools resolve only beneath the selected workspace. Users may
explicitly select existing workspace folders elsewhere, including an AFP
volume, without importing or moving them into Workspaces. Their sessions and
retained MCP results are associated with their workspace IDs in the data root.

Keep preferences and credential-bearing MCP configuration in the Preferences
folder. Remember the chosen data root and active workspace selection there.
Keep the workspace registry with the data root so its stable IDs and record
associations survive copying that folder; validate or explicitly relink folder
references on another disk or computer. The visible data folder is not a
credential store. Folder names in this layout
describe the application-owned structure, not literal names for OS special
folders, which are found through `FindFolder`.

Independent upgrades, ordinary Finder backup of a visible data folder, and
continued use of local or network workspaces tip the balance. A combined
installation folder couples application replacement to personal data;
Application Support hides working documents among installed support files;
workspace-local records do not provide the requested single home for history
and application diagnostics. A portable arrangement may be explicitly chosen
by locating the data folder beside the app, but is not the default and does
not merge their ownership or tool authority.

## Consequences

### Positive

- Application updates leave workspaces and retained records intact.
- One visible data folder contains managed workspaces and application records
  and can be backed up through the Finder.
- Local diagnostics can record loss of a network workspace when the chosen
  data root remains available; app launch location does not govern writes.
- The model's workspace boundary remains independent of the storage layout.

### Negative

- Workspace selection, stable association storage and first-launch folder
  setup add native UI and File Manager work.
- Copying a workspace folder alone does not include centrally stored sessions
  or MCP results. Copying the data root excludes external workspace contents
  and preferences/credentials; neither is automatically a complete backup.
- Saved handoffs outside the active workspace need an explicit application
  access path; existing relative file tools cannot simply read them there.
- Existing storage callers and installations need a deliberate transition;
  this ADR records the target design, not implemented runtime behavior.

### Risks

- A missing data volume can prevent journaling even when the workspace is
  readable. Report the failure and preserve the execution barrier rather than
  silently writing to another folder.
- Renames, moves and copies can break or duplicate workspace associations.
  Use stable IDs plus validated native folder references; detect missing or
  conflicting identities and require explicit user selection to resolve them.
- Choosing an overly broad workspace could include private records. Reject
  roots that contain the data root's application-owned record folders, and
  protect those records by catalog identity, not only by their names.

## Pros and Cons of the Options

### Separate application and user-selectable data folder

- Good: independent upgrades, visible records, selectable local/network storage.
- Bad: additional folder associations and migration behavior to maintain.

### Application and mutable data in one installation folder

- Good: one folder to copy for a self-contained portable installation.
- Bad: upgrades can replace data; writes depend on the application's volume;
  shared or read-only application locations become unsuitable defaults.

### Mutable data in System Folder:Application Support

- Good: a recognized home for installed application support files.
- Bad: working projects and conversation archives are harder to discover and
  back up, and differ from the installed support role described by Apple.

### Keep all records inside each workspace

- Good: copying a workspace can carry its projects and associated history.
- Bad: records are scattered, and network workspace loss also removes the
  place to record that failure; there is no single application data folder.

## Implementation Notes

- Centralize bounded data-root and workspace resolution rather than letting
  `main.c`, `session.c` and MCP persistence each invent paths. Validate composed
  MacRoman colon paths against existing buffer and File Manager limits; fail
  explicitly when a folder or path cannot be represented.
- Use `FindFolder` for Documents and Preferences. Native folder selection and
  Alias Manager references may help track moved roots; this application-owned
  resolution does not relax ordinary file tools' refusal to traverse aliases.
- Expose New Workspace, Open Workspace and Show Sherclawk Data in Finder.
  Workspace selection takes effect with a new chat while execution is idle,
  so one conversation does not silently mix workspace guidance or authority.
  In-flight builds, open journals and retained results keep their original
  workspace association and storage location until closed. Launch diagnostics
  retain their storage location and identify the workspace on each applicable
  event, including after a workspace switch.
  Data-root changes require a quiescent transition; never rebind open records.
- Give application-owned records dedicated bounded readers where needed.
  Root selection grants no model access to other workspaces, raw journals,
  credentials or arbitrary data-root files. Diagnostic access follows
  ADR-0005's privacy and authorization rules. Do not load data-root guidance
  into the model as an additional global instruction source (ADR-0004).
- New Chat and relaunch never restore process ownership from stored records,
  steal build locks or replay uncertain work. Build queues, snapshots and
  artifact authority stay associated with their original workspace; moving
  those into central storage is not part of this decision.
- An unavailable selected data root has no automatic local fallback or mirror.
  Failed mandatory journaling stops execution; diagnostic failure alone leaves
  ordinary use available with a visible warning. An unavailable workspace does
  not redirect file tools, but need not stop diagnostics in an available data
  root. Retention only removes identified diagnostic records, never sessions,
  MCP evidence, projects or build artifacts.
- Existing saved workspaces must be offered for explicit adoption without
  silent import, deletion or moving their queues. Preserve old journals and
  handoffs as legacy evidence; any relocation is explicit, verified and does
  not overwrite originals. Record associations before new central writes.
- Release packaging and Finder-copy verification must preserve resource forks
  and type/creator metadata. Verify first launch, upgrade, renamed folders,
  external workspaces, unavailable volumes and storage failure on OS 9.2.2;
  keep host fault checks for the shared storage policy.
- Revisit if users need workspace folders to carry all history by ordinary
  Finder copy, or concurrent Sherclawk instances must write one data root.
  This layout alone does not establish safe concurrent writer ownership.

## References

- [Apple TN1151: Creating NetBoot Server-Friendly Applications](https://leopard-adc.pepas.com/technotes/tn/tn1151.html)
- [Apple Folder Manager: Folder Type Constants](https://dev.os9.ca/techpubs/new/FolderMgrRef/FolderMgrRef.d.html)
- [ADR-0001: Native ToolServer execution](0001-execute-builds-natively-through-toolserver.md)
- [ADR-0003: Read-only admission and journaled mutations](0003-admit-tools-read-only-first.md)
- [ADR-0004: Bounded owner guidance](0004-load-agents-md-as-bounded-owner-guidance.md)
- [ADR-0005: Bounded read-only self-diagnostics](0005-expose-bounded-read-only-self-diagnostics.md)
