---
title: Quit applications owned by Sherclawk
issue: 102
status: guest-verified
adrs:
  - ADR-0001: docs/adr/0001-execute-builds-natively-through-toolserver.md
  - ADR-0003: docs/adr/0003-admit-tools-read-only-first.md
---

# Quit applications owned by Sherclawk

Authority lasts for the current Sherclawk process, independently of chat state.
`run_application` enumerates a bounded prelaunch snapshot, confirms that the
returned PSN was absent, checks the authorized artifact and Sherclawk launcher,
and grants authority only after `run_observed` is durably journaled. Its registry
stores the run ID, exact PSN, artifact FSSpec, process launch date and attempt bit.
The registry is never persisted or reconstructed. Pre-existing processes never
acquire another close handle. Tracking failure does not prevent an otherwise
valid launch, but its result reports no quit support and explains why.

There are 32 ownership slots and a 256-entry process snapshot. Reclamation checks
one registry entry per turn, conservatively retaining observation failures;
enumeration advances one process per turn. Over-limit or failed snapshots fail
closed. Before Quit, identity and launcher are checked again, with explicit
self/Finder/system exclusions. Confirmed prior exit removes authority without
sending an event. Identity changes revoke authority.

`ae_dispatch.c` installs the sole answer handler and reserves monotonically
increasing positive signed 16-bit return IDs for both ToolServer and Quit. IDs
never recycle, including after dispatcher reinitialization. Correlation checks
both the typed return ID and exact typed sender PSN. Two callback slots allow
a draining ToolServer operation and a Quit operation to coexist. Forgetting a
callback cannot make its old ID reusable. Initialization failure, full callback
slots and ID exhaustion prevent sending. ToolServer still sends `misc/dosc`
with its existing script and parses `stat`, direct output and diagnostics.

Quit builds a PSN-targeted `aevt/quit`, records intent, and sends with
`kAEQueueReply | kAENeverInteract`; it supplies no save/discard parameter.
All descriptors are disposed on every construction/send path. If `AESend` was
called, the run is permanently marked attempted, even when delivery is unknown.
Construction errors before `AESend` remain retryable. Quit then checks presence
once per turn for 30 seconds. Disappearance wins over reply status; refusal is
an error only while the same process remains present. Acceptance without exit
remains pending. Malformed answers, uncertain observations, timeout, Stop after
send and post-send journal failure yield uncertain outcomes. No cancellation,
force quit, automatic cleanup or replay is performed.

The new generated starter handles Open Application and Quit and dispatches
high-level events. Quit sets its normal exit flag; the ordinary cleanup closes
its log and releases UI objects and handler descriptors. Previously generated
projects are unchanged. The starter remains within the existing 4096-byte text
limit.

Host checks exercise the production dispatcher against Apple-event stubs, not
just a mocked Quit transport. `SherclawkQuitCheck` is the OS 9.2.2 release gate:
an accepting but non-exiting app must time out, its subsequent pre-existing
launch must not get authority or receive another Quit, and a fresh generated
starter must exit normally. The October 9, 2026 pass is recorded in
`docs/history/verification.md`; the model schema and environment discovery now advertise `quit_application`.
