# 006 — Sherclawk performance tuning

**Status:** analyzed, not measured. Findings come from reading the current
source tree (October 6, 2026); no guest timings were taken for this
page. Per-item costs below are code-derived estimates, not measurements. The
shared TLS transport already had a measured performance pass
(`patches/certainly-*-perf-*.patch`, guest results in
`docs/certainly-perf-checks.md`); this page is about what remains.
Per AGENTS.md, host success is not guest evidence — every change here needs a
guest fixture and a before/after time.

## The idea

Sherclawk's compute core — JSON, agent history, text conversion — is already
bounded and small (`agent.h:5-12`). The remaining performance work is in the
cooperative I/O steppers, the polling loops, the fixed per-round latencies, and
the page sizes that decide how many model round trips a task needs. The guest
is an emulated G3 under QEMU (`docs/macos9-qemu.md`), so guest instructions, AFP
round trips and one-tick event turns are all far more expensive than on the
host; a change earns its place by removing those, not by micro-optimising
strings.

Rules for anything here:

- Measure on the guest first. No item below has a number attached yet.
- Keep every stepper bounded: one small operation per event turn, no blocking
  UI, Stop still means "no further steps".
- The journal, staging, verification and uncertainty contract does not change.
  FlushVol barriers, closed-file verify reads and whole-file revisions stay.
- Any model-visible limit change (log/read/search page sizes) is a product
  decision, not just a tuning knob: it changes what the model sees per result.

## What Sherclawk does today

- The main loop services one cooperative step per event turn while a run is
  active (`WaitNextEvent(..., gSending ? 1 : 10, ...)`, `main.c:1068-1075`).
  Builds and launches hold `gSending` in a dedicated state and step
  `build_project_step` / `run_application_step` (`main.c:976-991`).
- The job snapshot pipeline uses 1 KiB pages (`JOB_PAGE`, `jobs.h:9`), one
  open/close per page (`jobs.c:96-103`), write-then-verify-read for every input
  (`jobs.c:104-142`), one flush per completed input (`jobs.c:120-127`), and a
  result poll at most once per 60 ticks plus bounded log pages
  (`jobs.c:181-194`, `jobs.c:212-213`).
- `build_project` pre-reads every descriptor/input **twice** (compare pass) in
  the same 1 KiB pages before publication (`build_project.c:228-250`).
- `run_application` verifies both forks of the recorded artifact in 1 KiB
  pages, hashing every byte with FNV — once at authorization and again at
  launch (`run_application.c:146-166`), with a 1 MiB per-fork limit
  (`run_application.c:12`).
- `read_build_log` returns 128-byte pages (`build_project.c:351-363`);
  `search_text` reads at most 8 KiB and 64 catalog entries per call
  (`tools.c:313-320`); `read_text` scans at most 8 KiB (`tools.c:228`).
- Every model round pays a full OT teardown: two 60-tick yields plus
  `CloseOpenTransport` (`main.c:180-186`), then `InitOpenTransport` and a fresh
  TLS handshake for the next request (`main.c:937-952`). Up to 16 rounds per
  run (`AGENT_TURN_MAX`).
- `worker.pl` scans and sorts the whole queue directory every second
  (`worker/worker.pl:67-78`), and completed job folders are retained for
  inspection (`README.md:194`), so that scan grows for the life of
  the installation.
- The transport already has its measured pass: HKDF/HMAC setup reuse, AES/GCM
  context reuse, and same-pump advancement after a completed send
  (`patches/certainly-tls13-perf-keysched-hmac.patch`,
  `certainly-tls13-perf-records-aes-gcm.patch`,
  `certainly-perf-transport-pump.patch`); guest workloads measured 1.43–1.9x
  on key setup and small-record AES, with large records within noise
  (`docs/certainly-perf-checks.md`, "Verified results, 2026-10-01").

## 1. Cooperative stepper granularity — the same pattern in three places

`jobs.c`, `build_project.c` and `run_application.c` all walk files in 1 KiB
pages, reopening the file for each page. That bounds a step, but it multiplies
AFP round trips and event turns: a small multi-file project's snapshot costs
well over a hundred page transfers before the worker even sees the job, and
each fork scan of a ~100 KiB artifact costs ~100 event turns (~1.5–2 s at the
1-tick cadence) per phase.

Candidates, in increasing risk:

- Raise the bounded page (2–4 KiB). Still exactly one bounded operation per
  event turn, just a larger one. The bound is pinned by
  `tests/test_jobs.c:103` (`read_bytes<=2*JOB_PAGE+JOB_RECORD_MAX`) and
  documented in `jobs.h:42-45` and `worker/README.md`, so both move together.
- Keep the open reference across consecutive pages of the same file instead of
  open/close per page. `jobs.c` documents closed-file range I/O deliberately
  (`jobs.c:92`), so this one needs a decision, not just a patch.
- Merge the fork scan with the final metadata recheck (hash while rechecking
  once) — but `run_application` rescans on purpose because the artifact may
  change between authorization and launch; that is a correctness trade, not a
  free win.

Do not "tune" away the duplicate pre-read pass in `build_project.c:241-250`
or the staged write/verify in `jobs.c:104-142`: those are the revision and
exact-bytes guarantees, not overhead.

## 2. Worker queue scan growth

`worker.pl` runs `readdir` → regex filter → `sort` → per-entry `-e`/`-l` stats
every second (`worker/worker.pl:67-78`). Job folders are never cleaned up, so
the cost grows without bound and directly competes with the app under OS 9's
cooperative scheduling. Order is irrelevant under the singleton worker lock —
only jobs whose `ready` appears need claiming. Options: drop the `sort`, stat
only plausible pending entries, or keep a cursor/marker the producer updates.
`tests/test_worker.py` already exercises this file on the host. Lowest-risk
item on the page; verify with a retained queue of, say, a few hundred jobs and
a tick counter around one scan.

## 3. Model round-trip page sizes

Every extra page is a full TLS handshake plus a model inference round, which
dwarfs the guest CPU cost of a larger page. `read_build_log`'s 128 bytes
(`build_project.c:351`) is the extreme case: a 1 KiB compiler log is eight
model turns. The 1536-byte result cap (`agent.h:6`) leaves headroom — 128 raw
bytes worst-case-escape to roughly 500–800 output characters — so ~256 bytes
should still fit with an explicit worst-case proof. `search_text` (8 KiB,
64 entries, 4–8 matches) and `read_text` (8 KiB, 20 lines) have the same
shape: bounded for Stop responsiveness because a File Manager call cannot be
interrupted, but potentially conservative for round-trip count.

Decide with data: how many `read_build_log`/`read_text`/`search_text` calls the
accepted fixture conversations actually used, and how much of each result was
consumed. Raising a bound is only a win if the model stops paging early.

## 4. Fixed per-round latency: the OT teardown

`CloseChatContext` is the largest fixed cost per model round: two 1-second
yields plus a full `CloseOpenTransport`, then a fresh `InitOpenTransport` and
TLS handshake for the next request (`main.c:180-186`, `main.c:937-952`,
`main.c:1041`). A 16-round run can spend ~32 s in deliberate waiting before
counting the handshake. The comment says the cycle exists because OS 9's OT
wedges after a mid-flight failure; that is load-bearing experience, and the
starting-point fixes this workspace already records are not to be re-litigated
casually.

The only honest experiment: instrument when the wedge actually appears, then
try (a) shorter yields and (b) cycling OT only after a failure, with a soak
test long enough that an intermittent wedge has a chance to show up. One clean
run proves nothing. If either works, it saves wall-clock on every round of
every run.

## 5. UI and micro loops (last)

All bounded; do these only if measurements point at them:

- Status line redraws whenever the received byte count changes
  (`main.c:1023-1027`, `SetStatus` at `:121`) — up to ~60 invalidations/s while
  the body streams. Throttle to e.g. 1 KiB steps.
- `ShowMessage` scans the whole transcript twice, re-`strcat`s it four times
  and hands the whole 30 KB to `TESetText` (`main.c:786-810`,
  `main.c:340-354`), re-splitting every line per appended message.
- `search_text`'s substring scan is a naive `memcmp` at every offset
  (`tools.c:433`); a skip-table matcher would cut worst-case compares, but the
  8 KiB budget and AFP catalog calls dominate.
- Reverse MacRoman lookup linearly scans 128 entries per non-ASCII character
  (`text.c:49`); mutations are rare and ≤4 KiB.
- `json_member` re-decodes every key on each lookup (`json.c:188-199`) and
  `agent_request` re-copies/quotes the full 256 KiB history every round
  (`agent.c:110`); both are bounded and fine at current sizes.

## 6. Deliberate costs — do not tune casually

FlushVol per journal record (`main.c:783`), per log line for host tailing
(`main.c:238`), per completed input in staging (`jobs.c:120-127`); the
closed-file read-verify cycles; the 1-tick event cadence while sending
(`main.c:1068-1075`); and the small result pages in §3 that keep Stop and the
UI responsive. Each is a crash-evidence, exact-bytes, or responsiveness
guarantee. Changing one changes the documented contract, not just the timing.

## Measurement plan

- Guest timing in the established pattern (the `guest.c` workload described
  in `docs/certainly-perf-checks.md`): fixed workloads, tick counts
  written to a share log, `DONE failures=0`. Add counters around (1) a full
  snapshot publication, (2) the `run_application` fork scan, (3) one worker
  idle scan with N retained jobs.
- Existing diagnostics already produce the right fixtures:
  `SherclawkJobCheck` (3 KiB snapshot + log pages),
  `SherclawkBuildCheck` (snapshot → worker → log continuation), and the
  recorded build conversations. Correlate recipe `stage=` markers with
  timestamps for per-phase wall clock.
- Host: `tools/check.sh` (ASan/UBSan) after any bound change; update
  `tests/test_jobs.c` alongside `JOB_PAGE`; a matcher change gets its own
  fixture assertions. `tests/test_worker.py` covers the worker scan change.
- Acceptance for any change: the same fixture reports `RESULT failures=0` and
  the measured tick/wall-clock number improves; otherwise revert. A single
  fast run on the host or in the guest is not evidence of a fix (AGENTS.md).

## Recommended sequence

1. **Instrument first.** Snapshot ticks, fork-scan ticks, worker idle-scan
   ticks, per-phase build wall clock. Priorities depend on where the time is.
2. **Worker scan fix** (§2). Independent, host-testable, no contract change.
3. **Stepper page/handle experiment** (§1) behind the existing tests and doc
   updates.
4. **Round-trip page sizes** (§3) with a worst-case fit proof and evidence
   from the fixture conversations.
5. **OT yield/cycle experiment** (§4) with a guest soak.
6. **Micro items** (§5) only if the numbers say so.

## Open questions

- Primary goal: guest UI responsiveness, build wall time, or end-to-end model
  rounds? The ordering changes the answer.
- Where does build time actually go — snapshot prep, worker pickup (≤1 s sleep
  + 60-tick poll), MrC/Rez, or log continuation?
- How large do retained queues get in practice? The worker scan fix matters
  little at tens of jobs and a lot at thousands.
- Are the 60-tick yields empirical, or conservative? Does the wedge ever occur
  on clean completions, or only after mid-flight failures?
- Should `run_application` reuse the authorization scan instead of re-hashing
  both forks at launch (metadata-only recheck plus one final hash pass), or is
  the current double scan the intended strength?
- Is changing model-visible page sizes acceptable at all, given each is also a
  Stop-granularity and result-cap guarantee?
- Does `JOB_PAGE` appear in any compatibility contract beyond this repo's
  tests and docs? `worker.pl` consumes whole files, so the app and host tests
  are the only consumers.

## Evidence

- `jobs.h:9,42-45`; `jobs.c:92-142,181-194,212-213`;
  `tests/test_jobs.c:103`.
- `build_project.c:228-250,351-363`.
- `run_application.c:12,146-166`.
- `main.c:121-130,180-186,238,340-354,786-810,937-952,976-991,
  1023-1027,1041,1068-1075`.
- `tools.c:228,313-320,419,433`; `text.c:49`;
  `json.c:188-199`; `agent.c:110`; `agent.h:5-12`.
- `worker/worker.pl:67-78`; `README.md:190-207`;
  `worker/README.md`.
- Transport baseline and prior measured work:
  `patches/certainly-tls13-perf-keysched-hmac.patch`,
  `certainly-tls13-perf-records-aes-gcm.patch`,
  `certainly-perf-transport-pump.patch`;
  `docs/certainly-perf-checks.md` (host checks and verified results).
- `docs/macos9-qemu.md` (emulated G3); `AGENTS.md` (guest verification rule).
