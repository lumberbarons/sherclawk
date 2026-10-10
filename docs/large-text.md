---
title: Robust editing of files up to 64 KiB
status: implementation-awaiting-guest-acceptance
issues: [20, 115]
adrs: [ADR-0001, ADR-0003]
---

# Robust editing of files up to 64 KiB

The implementation supports 65,536 encoded bytes through search, verified
read, unique exact edit, readback and native build. Shipping Sherclawk retains
its guest-verified 4 KiB file/input limit. `SherclawkLargeTextCheck`,
`SherclawkEditCheck` and the build diagnostics opt into 64 KiB; host checks
exercise both modes. Set `SHERCLAWK_LARGE_TEXT_VERIFIED` in `text_limits.h` only
after the OS 9.2.2 evidence below is recorded. Schemas, environment fields and
validation use the same selected limit.

[ADR-0001](adr/0001-execute-builds-natively-through-toolserver.md) constrains
native build execution, ownership and no replay.
[ADR-0003](adr/0003-admit-tools-read-only-first.md) constrains mutation journals,
immutable execution evidence, bounded results and the guest admission gate.

## Read and edit

`read_text_begin/step` and `edit_text_begin/step` use the pending-tool contract:
2 pending, 0 completed, 1 stop the run. They share one 64 KiB source buffer;
a second 64 KiB buffer retains edited bytes. Verification uses 1 KiB scratch.
Only one text operation may be active. Both have a wrap-safe 60-second deadline.
The dispatch entry points enforce argument and execution-evidence guards.

Each file transfer, splice and verification advances at most 1 KiB. KMP prefix
construction and matching charge each comparison, including fallback, against
a 1,024-comparison step budget. Overlapping matches count separately; the second
match refuses the edit immediately. Strings remain at most 4,096 MacRoman
bytes each, arguments 8 KiB, and results 4,096 bytes. Multiple replacements per
call remain outside this change (#113). Create-only writes remain 4 KiB.

Within the selected file limit, reads retain the entire snapshot, validate and
hash chunks, reread and compare exact bytes, check catalog identity/metadata,
and close successfully before returning a `full-...` revision. Its format and
FNV hash remain unchanged. LF is readable but makes the whole file uneditable.
Pagination never changes the revision. Line navigation spans the snapshot;
pages measure converted/escaped bytes against their entire JSON envelope and
never split CRLF pairs. Partial lines continue with `next_byte`.

Above the selected limit, reads remain bounded 8 KiB observations and return
`scan-...`, never an edit guard. Revisions are not cached between calls and are
change detectors, not cryptographic signatures.

Edits retain an exclusive File Manager open on the original through:

1. Intent journal; collision-safe sibling creation; chunked write, close, flush
   and exact stage verification.
2. Staged journal; original identity/metadata and exact-byte recheck.
3. Backup rename attempt, flush, identity/metadata and exact-byte verification;
   backed-up journal.
4. Publication rename, flush, identity/metadata and exact-byte verification;
   successful close; committed journal.

Recovery-result capacity is checked before creating a stage. The original
Finder metadata stays with the backup; the replacement is `TEXT`/`ttxt`.
Before the first rename attempt, failure or Stop closes handles and leaves the
original at its path, reporting any retained stage. From that attempt onward,
failure or interruption is uncertain and stops the run, with all recovery
paths. There is no rollback, cleanup, replay or retry. Direct AFP-server writers
bypass File Manager sharing; this is not filesystem transactionality.

## Build snapshots and memory

C, header and Rez inputs may each reach 64 KiB in acceptance builds; the
4 KiB descriptor and ten-input limit remain. A 128 KiB immutable arena replaces
per-input arrays; recorded pointers/lengths are retained until jobs finish.
Each first-pass chunk validates and hashes; the second pass compares bytes and
catalog identity. Arena capacity and the actual aggregate (descriptor, inputs,
recipe and manifest) are checked before queue creation, snapshot journaling or
reservation. The total remains 128 KiB. `SNAPSHOT_SIZE_LIMIT` identifies the
applicable descriptor, input and total limits.

Sherclawk's partition is 10 MiB minimum / 16 MiB preferred. Generated starter
partitions are unchanged. Linked size is recorded in `limits.md`; guest heap
headroom is an acceptance measurement, not inferred from static size.

## Acceptance evidence still required

Host coverage includes cap boundaries, unique/overlapping/repetitive matches,
chunk crossings, pagination reconstruction, stale whole-file revisions, late
I/O faults, every edit journal/rename boundary, cancellation through phases,
tick wrap, recovery handles, immutable build inputs and aggregate rejection.
Both clang and GCC run with ASan/UBSan.

Run the edit diagnostic on the AFP workspace at the 10 MiB minimum. Preserve its
log, fixture and backups. It writes 16/64 KiB fixtures natively, times every
phase/step, reports heap headroom, checks boundary-crossing replacements and
exact backup/replacement bytes, and rereads fresh revisions. Record visible
responsiveness and Cmd-period Stop before and after a rename.

Run `SherclawkBuildCheck` (or `SherclawkSelfBuildCheck`) with idle ToolServer.
Its independent project includes a 64 KiB source. Repair keeps the file exactly
64 KiB and compiles a fresh snapshot through ToolServer. Preserve build logs.

Finally run `SherclawkLargeTextCheck` and request a model-driven literal
search/read/edit/read/build against that project. Preserve the model journal,
arguments/results and build evidence. Record timings, maximum step duration,
linked size, heap headroom and outcomes in `history/verification.md` before
enabling the shipping gate. Host success is not guest evidence.
