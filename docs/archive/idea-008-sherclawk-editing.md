# 008 — Sherclawk editing: larger files, several hunks, one transaction

> Historical design note. Proposals, status statements and source-line references
> describe development at the time of writing and may be superseded. Use the
> [current guides](../../README.md#documentation) for supported behavior and the
> [issue tracker](https://github.com/lumberbarons/sherclawk/issues) for active work.

**Status:** idea, not built. Findings come from reading the current source
tree (October 6, 2026); no guest timings were taken and nothing here has been
compiled or run. The behavior described under "What Sherclawk does today" is
the implemented, guest-verified ≤4 KiB contract; every proposal below still
needs host checks and a real OS 9.2.2 run (AGENTS.md).

## The idea

`edit_text` today makes exactly one exact-text replacement in a MacRoman/CR
file of at most 4,096 bytes. That contract is deliberately small, exact and
journaled. Grow it in three directions — larger files, multiple replacements
per call, and explicit bulk/whole-file operations — without relaxing any of the
guarantees that make edits safe: whole-file revision guards, exact matching,
strict encoding, verified staging and retained backups, explicit uncertain
outcomes, no silent retries.

The target is not a text editor or a shell. It is the minimal set of editing
primitives a coding harness needs when the file is bigger than one model
argument.

Why this is the next gap:

- **Files outgrow the cap fast.** The app's own sources run 20–58 KB
  (`agent.c` 20,568; `main.c` 43,200; `tools.c` 57,989 bytes), and
  `templates/ppc-toolbox` projects are capped at five 4 KiB files, so any
  project that grows past that wall stops being editable and buildable
  together.
- **Reads page; edits don't.** `read_text` scans 8 KiB per call with byte and
  line continuation, but a file over 4 KiB returns only `scan-` tokens that no
  mutation accepts. The model can inspect a file it can never fix.
- **One match per call is one transaction per match.** Several calls in one
  model response cannot batch a change, because the second call's
  `expected_revision` does not exist until the first commits. Every related
  replacement pays a full stage → verify → backup → publish cycle plus a model
  round, and each round is a fresh TLS request (idea 006 §4).
- **The guards scale; the plumbing doesn't.** A whole-file hash of 16–64 KiB
  is still a cheap, bounded guard; what's missing is streaming the match, a
  revision path above the cap, and a transaction that can carry more than one
  replacement.

Out of scope here: move/rename/copy/delete and diff as standalone file
operations — idea 004 already scopes those as separate general-purpose gaps. A
bounded change report and a read-only compare appear only as verification
support for larger edits.

## What Sherclawk does today

- `edit_text(path, expected_revision, old_text, new_text)` replaces exactly one
  nonempty unique match; empty `new_text` deletes. Repeated or overlapping
  matches (`AMBIGUOUS_MATCH`), a missing match, an unchanged result, a stale
  revision, LF/CRLF sources, binary controls, aliases, resource forks and
  over-limit files all refuse before staging (tools.c:600-767).
- Both the source and the edited result must fit 4,096 bytes; edits require a
  `full-` revision that only reads of files ≤4 KiB produce. Larger reads return
  `scan-` observational tokens, which edits never accept (tools.c:202-280;
  `full_revision` at :185-189; README "Editing text" :377-412).
- The mutation tail is the safe part: exclusive open, journaled
  intent/staged/backed-up/committed, verified staging, `FlushVol`, rename
  publication, retained backup, and explicit uncertain outcomes that stop the
  run. This is what must not change (tools.c:669-767).
- Limits: arguments 8 KiB per call, results 1,536 bytes, four calls per
  response (agent.h:5-12). Tool calls run synchronously; "these small
  synchronous writes finish before another UI event is handled" (README :108-109).
- Tests already pin the whole contract: `tests/test_tools.c:175-243` models the
  edit path and its faults; `tools/edit-check.c` is the guest diagnostic; the
  schema and policy text live in `agent.c:58-63` and the `edit_max_bytes`
  report in the environment response (tools.c:113).

## Design rules that do not change

- The whole-file revision is the only mutation guard; `scan-` tokens never
  authorize a change.
- Exact bytes or explicit failure — no fuzzy matching, no positional guessing
  unless a later candidate earns it.
- Strict MacRoman, CR text, binary controls refused; silent LF conversion
  stays forbidden.
- One journaled mutation per call: intent, staged, backed-up, committed.
  Staging/verification/publication and retained backups are untouched; no
  automatic retry or rollback.
- Every step bounded, every result within `AGENT_RESULT_CAP`, Stop semantics
  defined per step.
- Keep complexity in deterministic code (research :81) — a small engine in the
  app, not prompt conventions.

## Candidate 1 — raise the editable cap, same semantics

Keep one unique exact replacement, but allow the file up to a new cap
(16–64 KiB is the plausible band), the replacement still ≤4 KiB, and total
growth bounded by the cap.

- Requires: a whole-file revision above 4 KiB, a streaming unique-match scan
  (page through the file, carry `old_len−1` bytes across page boundaries, the
  same discipline `search_text` already uses), and a bounded splice. The
  stage/verify/backup/publish tail is unchanged.
- Revision access is a real decision: extend `read_text` to return whole-file
  revisions up to the cap (hashing internally in bounded reads), or add a
  small read-only `get_revision(path)`. Read is the natural place — `editable`
  should mean "CR text within the editable cap", not "≤4 KiB" — but it makes
  every large read hash the file. Decide on measured ticks, not intuition.
- Cost check: FNV over 64 KiB plus AFP reads is plausibly sub-second, but the
  edit is synchronous today. Measure; if a cap size blocks the UI, the edit
  becomes a cooperative stepper like `build_project`/`run_application`
  (main.c:976-991), with Stop meaning "no further steps", not "abort the
  current page".
- Memory: `edit_text` uses five 4,097-byte statics; a 64 KiB cap needs roughly
  320 KiB of working buffers (static or allocated), so the `SIZE` math in
  README:466-478 should be re-measured the way the history decision was.

## Candidate 2 — multi-edit: several replacements, one transaction

The highest-value, lowest-risk item, because it needs no streaming.

```json
{"path":"Spikes:ccapp:main.c","expected_revision":"full-...","edits":[
  {"old_text":"return 0;","new_text":"return 1;"},
  {"old_text":"Greeting(\"hi\")","new_text":"Greeting(\"hello\")"}]}
```

- Semantics: prefer **simultaneous** — find every `old_text` uniquely in the
  original snapshot, require disjoint byte ranges, sort by offset, splice once.
  Sequential semantics (each edit matching the previous result) is easier to
  explain but can silently retarget text introduced by an earlier edit. Refuse
  overlap; dependent changes belong in one replacement.
- All-or-nothing: any missing, ambiguous, stale, malformed or oversized edit
  refuses the whole call before staging; the source is unchanged. This is
  exactly today's pre-stage behavior applied to a set.
- Benefits: one revision, one backup, one journal, one stage/verify/flush/
  publish cycle per logical change; fewer model rounds; fewer TLS handshakes.
- Bounds: cap the number of edits (e.g. 8) and the total `old_text`+`new_text`
  bytes; report an aggregate plus a bounded per-edit summary within 1,536 bytes.
- Parser work: `valid_keys`/`JsonToken[128]` must validate a nested array and
  still reject unknown or duplicate fields and oversized arrays
  (tools.c:940-971, agent.c:58-63).
- Shape question: grow `edit_text` with a required `edits` array, or add a
  second tool name and leave the single-pair form untouched. The schema is
  presented to the model every run, so a clean evolution is available either
  way.

## Candidate 3 — explicit occurrence control

Today any repeated match is `AMBIGUOUS_MATCH`. An explicit `occurrence: "all"`
(or an index plus expected count) makes bulk renames deterministic: count
first, refuse overlaps (`aa` in `aaa` still ambiguous), splice, report the
count, bound growth. The default stays unique; bulk is always opt-in and never
inferred.

## Candidate 4 — whole-file replace

`replace_text(path, expected_revision, text)` overwrites a bounded file without
match semantics — the natural operation for generated files and complete
rewrites, with the same mutation tail. It is nearly `edits` with the entire
content as `old_text`, minus the cost of sending the old content; decide
whether it earns a distinct tool name or rides candidate 2 as a replace-entire
mode. `write_text` stays create-only and unchanged.

## Candidate 5 — line/range edits and appends (probably later)

- `replace_lines(path, expected_revision, start_line, end_line, text)` is
  weaker (positional, not byte-exact) but handles repeated lines and large
  deletions with small arguments. The revision guard covers drift between read
  and edit; it is still a second editing model. Only add it if fixture
  conversations show exact matching cannot serve a real case.
- Append-at-EOF is the natural way to grow a file beyond one argument's worth
  of text, but each append is O(file) staging. If added, bound the append size
  and keep the whole-file revision guard; it also decides how new files above
  `write_text`'s 4,096-byte limit get created.
- Recommendation: defer both. Multi-edit usually solves repeated lines by
  including enough context to be unique.

## Candidate 6 — patch/diff application

The modern-harness shape (unified diff or a minimal context-hunk format) is
compact for large changes, but the applier would still be exact-match under the
hood; CR text and strict MacRoman fight diff conventions, and malformed
generated diffs are a real failure class. Defer; multi-edit is this idea with
less syntax risk. Revisit only with evidence that argument size, not round
count, is the binding constraint.

## Candidate 7 — change report and read-only verification

- Every result currently reports `revision`, `previous_revision` and
  `backup_path` (tools.c:581-593). A bounded change report (per-edit position
  plus a short before/after excerpt) makes large edits auditable in one turn
  instead of a follow-up read.
- A read-only `compare_text(a, b)` (current file against a retained backup)
  is the natural verification tool once edits above 4 KiB matter; it should
  page like `read_text` and never accept `scan-` tokens.
- Both stay inside `AGENT_RESULT_CAP`: positions and counts, not full diffs.

## What this unblocks — and what it does not

- It does not make Sherclawk self-hosting by itself: build inputs are capped at
  4 KiB per file and five files per project (README:169-176), and the native
  TLS/toolchain questions stand (research :682-684). But it removes the first
  and most obvious wall — sources you cannot edit — and the cap decisions for
  edit, read and build inputs should be made together, not independently.
- For ordinary conversations it lowers round counts: one multi-edit call
  replaces several single edits and their TLS round trips.

## Recommended sequence

1. **Multi-edit at the current cap** (candidate 2) — smallest change with
   immediate value; no streaming, no new revision path. Exercise overlap,
   disjointness, ordering, stale and oversized cases in `tests/test_tools.c`
   and `SherclawkEditCheck`.
2. **Occurrence control** (candidate 3) on the same engine.
3. **Acquire data before raising the cap:** how large accepted fixture files
   actually get; measured ticks to hash 16/64 KiB in the guest; whether the
   synchronous path hurts responsiveness. Then pick the cap deliberately.
4. **Raise the cap** (candidate 1) with the revision-access decision (read vs
   `get_revision`), streaming match and bounded splice; synchronous first, a
   cooperative stepper only if the measurements demand it.
5. **Whole-file replace** (candidate 4) as a mode or tool.
6. **Verification polish:** change report, `compare_text` (candidate 7).
7. **Defer** line/range edits and diff/patch (candidates 5–6) until fixture
   evidence.

## Verification when built

- Host: extend the File Manager model in `tests/test_tools.c` (edit block at
  :175-243) with page-boundary matches, multi-edit semantics, replace-all
  counts, growth limits, nested-argument rejection and worst-case result-cap
  envelopes, plus the existing fault injections (short writes, corrupt reads,
  rename races, changed-after-stage, journal barriers). `tools/check.sh`
  ASan/UBSan must stay green; `tools/edit-check.c` gains the new fixtures.
- Guest: extend the fixed `SherclawkEditCheck` diagnostic (README:553-554)
  with a >4 KiB file and a multi-edit fixture, recording ticks per phase; then
  a live main-app conversation that edits a file above the old cap and reads
  it back. Per AGENTS.md, host success is not guest evidence.
- Documentation moves together: README "Editing text" (:377-412), the limits
  list (:454-487), `edit_max_bytes` in the environment response (tools.c:113),
  and the schema/description in `agent.c:58-63`.

## Open questions

- Cap size, and whether it moves together with the build-input limits (4 KiB
  per file, five files) or edit-only first.
- Whole-file revision above the cap: hashed inside `read_text` or a dedicated
  `get_revision(path)`?
- Synchronous single call vs cooperative stepper at each cap — what do guest
  ticks say?
- Confirm simultaneous (disjoint, sorted) multi-edit semantics rather than
  sequential.
- Schema evolution: extend `edit_text` with `edits`, or a new tool name?
- Is an explicit journaled LF→CR conversion ever wanted, or does refusal stay
  absolute? (Silent conversion stays forbidden either way.)
- Backup accumulation above 4 KiB — is `move_to_trash` (idea 004 §6) enough
  before larger backups become expensive?
- Change report now, or revision plus backup path with a follow-up read?

## Evidence

- `tools.c:185-189` (`full_revision`), `:190-201` (`same_file`),
  `:202-280` (`read_text`; whole ≤4096 at :223, scan token :245-246, LF
  disables editability :236), `:600-767` (`edit_text`), `:940-971` (dispatch
  and worker-queue protection), `:111-113` (environment tool list,
  `edit_max_bytes`).
- `agent.c:58-63` (edit schema), `:12` (policy text);
  `agent.h:5-12` (bounds).
- `README.md:377-412` (Editing text), `:454-487` (limits), `:95-96`
  (`write_text` 4,096), `:169-176` (build inputs 4 KiB ×5), `:466-478`
  (static-buffer memory math).
- `tests/test_tools.c:175-243` (edit contract and fault model);
  `tools/edit-check.c`, `tools/check.sh`.
- `main.c:976-991` (cooperative build/run steppers) as the pattern
  if an edit must step.
- `docs/idea-004-sherclawk-os9-tools.md:21-22` (general-purpose gaps are
  separate); `docs/idea-006-sherclawk-performance-tuning.md` §1 (page/handle cost)
  and §3 (round-trip page sizes).
- `docs/sherclawk-agent-harness-research.md:81` ("keep that complexity in
  deterministic code"), `:682-684` (native rebuild toolchain questions).
- `PLAN.md:87-88` ("larger-file read revisions are still
  observational").
