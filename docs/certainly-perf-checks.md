# Certainly performance checks

Three independently investigated patches are retained: HMAC setup reuse
(`patches/certainly-tls13-perf-keysched-hmac.patch`), AES/GCM setup reuse
(`patches/certainly-tls13-perf-records-aes-gcm.patch`), and same-pump
advancement after completed sends (`patches/certainly-perf-transport-pump.patch`).
The Certainly checkout and its BearSSL checkout remain pristine. These
checks target Certainly revision `1cb584854ad0098074380a416015f3d3a7768b1f`.
The baseline includes the build prerequisite and the three existing
correctness patches, so timings do not compare against the broken upstream
IV or authentication paths.

Originally the `hello-https/tools/perf-tests/README.md` of the imac
workspace; the runners (`run.py`, `keysched.py`, `run-record-perf.py`,
`run-transport.sh`), the guest workload (`guest.c`) and the recorded logs
stay there, and the paths below are relative to the original `hello-https/`.
The coverage and the measured results are kept as written.

## Host checks

Run from the original workspace's `hello-https/`:

```bash
python3 tools/perf-tests/run.py
python3 tools/perf-tests/run.py --sanitize
```

All runners resolve the Certainly checkout through `--certainly` or
`CERTAINLY_DIR` (default `../Certainly`), and the shared table in `common.py`
decides which patches each comparison withholds, so one run can never mix
checkouts or silently skip the optimization under test.

The runner force-rebuilds and runs Certainly's entire current host suite
(three key-schedule checks and 58 HTTP assertions) on the combined patch
set, then runs the following checks. Outputs stay under ignored `build/`.
Each stage is isolated so `git apply` cannot silently skip files while
searching the parent workspace's Git repository.

| Runner | Coverage |
|---|---|
| `keysched.py` | 768 randomized differential/guard cases; independent Python HMAC/SHA vectors for SHA-256/key16, SHA-256/key32 and SHA-384/key32; full-schedule CPU timings |
| `run-record-perf.py` | Three AEAD suites; 14 sizes through 16 KB; sequence boundaries; exact baseline ciphertext; modified tags/ciphertext, wrong keys, short records and AAD length; padding, retries, copies and rekeys |
| `run-transport.sh` | Partial/blocked writes, retry byte integrity, send/receive failures, handshake buffer ownership, final Finished, TLS 1.2 fallback and engine closure; at most one send/receive/handshake step per pump |

The optimized host runs (`cc -O2`) measured approximately 1.17–1.19x for
full key schedules and 1.49–1.52x for 64-byte AES encrypt/decrypt pairs.
AES at 1 KB improves about 1.06x; full-size records and ChaCha remain
within timing noise. Record timings exclude once-per-key initialization.
Sanitized results establish correctness, not performance.

The pump comparison is deterministic: a fully sent TLS 1.3 flight can
advance in one pump rather than two; TLS 1.2 can receive after acknowledging
a send in the same pump. A partial send still yields. This removes an
event-loop wait at those transitions, not an entire network round trip.

## Guest checks

`guest.c` is an ordinary Retro68 application with no interactive window.
It writes `Retro68:CertainlyPerf.log`, exercises fixed-work crypto loops,
fetches three endpoints with the real Open Transport implementation, and
quits automatically. The mounted AFP volume is required. It does not alter
VM configuration or restart netatalk.

```bash
# From the original workspace's hello-https/: build and publish the
# combined-patch harness.
BUILD_TARGET=CertainlyPerf_APPL APP=CertainlyPerf tools/deploy-to-share.sh
```

In Finder, open the Retro68 volume, select `CertainlyPerf`, and use Command-O.
If the newly published file is absent, close and reopen the volume window.
Wait for `DONE failures=0` before replacing the executable or its log.

For a baseline, make an ignored directory containing the build prerequisite
plus just the correctness patches, then select it with `PATCH_DIR`:

```bash
mkdir -p build/baseline-patches
cp patches/certainly-events-tickcount.patch \
   patches/certainly-tls13-chacha20-tag-check.patch \
   patches/certainly-tls13-hkdf-iv-overflow.patch \
   patches/certainly-tls13-msg-buf-overflow.patch build/baseline-patches/
PATCH_DIR=build/baseline-patches BUILD_TARGET=CertainlyPerf_APPL \
  APP=CertainlyPerf tools/deploy-to-share.sh
```

Run the baseline and save its log before deploying/running the combined
patch set. Both builds use identical workload source and Release flags.
Library source/header timestamps are renewed when staging to ensure patch
set changes rebuild every affected object and caller.

## Verified results, 2026-10-01

The complete host runner passed normally and with ASan/UBSan. Upstream
tests also passed after forced rebuilds on the correctness-only baseline.
The host socket rig fetched Postman Echo, Google's `/generate_204`, and
httpbin `/get` on both baseline and combined stacks; the combined rig was
sanitized. The full HelloHTTPS application and guest harness cross-compiled.

Mac OS 9.2.2 under the running UTM PowerPC VM passed both harness runs with
`DONE failures=0`: all record round trips and modified-tag rejection, plus
Postman Echo and Google over TLS 1.3/ChaCha, and httpbin over TLS 1.2 fallback.
HTTP status and expected response markers were checked, and responses were
drained to Content-Length completion or clean close. Public responses vary
in size; network latency measurements are not used as CPU benchmarks.

| Fixed workload | Baseline ticks | Combined ticks | Ratio |
|---|---:|---:|---:|
| 30,000 handshake/application key derivation pairs | 77 | 54 | 1.43x |
| AES-128, 100,000 empty encrypt/decrypt pairs | 80 | 43 | 1.86x |
| AES-128, 100,000 64-byte pairs | 112 | 74 | 1.51x |
| AES-128, 10,000 1-KB pairs | 57 | 54 | 1.06x |
| AES-128, 1,000 16-KB pairs | 79 | 81 | 0.98x |
| AES-256, 100,000 empty pairs | 99 | 52 | 1.90x |
| AES-256, 100,000 64-byte pairs | 138 | 91 | 1.52x |
| AES-256, 10,000 1-KB pairs | 71 | 67 | 1.06x |
| AES-256, 1,000 16-KB pairs | 99 | 99 | 1.00x |

Ticks are 1/60 second. These are single long-workload runs on an emulator,
not statistical estimates for real PowerPC hardware. Large records show
no meaningful gain. ChaCha code is unchanged; measured baseline/combined
ticks were 8/9, 12/13, 6/6 and 9/9 at the same four sizes. The guest
key benchmark repeats traffic-key derivation, whereas the host benchmark
includes the whole key schedule, explaining their different ratios.

Record contexts grow from 60 to 404 bytes on PowerPC: 688 additional bytes
per connection for the two directions. On the 64-bit host they grow from
72 to 440 bytes. Initialization pays the AES/GHASH setup once per key;
the repeated-record benchmarks start after that setup. The live guest
endpoints selected ChaCha; AES functionality on PowerPC was exercised by
the local record tests rather than a forced live AES negotiation.

Detailed run logs are ignored artifacts in the original workspace's
`hello-https/build/`: `perf-suite.log`, `perf-suite-sanitized.log`,
`guest-baseline.log`, and `guest-combined.log`.

## Other review findings

Static review found two existing application-write limitations outside
these performance changes. HelloHTTPS's `DriveFetchStep` sets
`gRequestSent` after one `MacTLS_Write` call even if it accepts only part
of the request. Certainly's TLS 1.3 `MacTLS_Write` also advances the record
sequence before sending and keeps pending ciphertext only on the stack:
a blocked send can discard a ciphertext suffix while reporting the whole
plaintext length, and a partial header retry can produce a new record.
The retained pump patch (`patches/certainly-perf-transport-pump.patch`)
covers handshake writes, not this application-data path. The small live
GETs did not trigger these limitations; fixing them would require a
separate buffered-write change and backpressure tests.