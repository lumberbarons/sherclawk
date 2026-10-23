# Unattended diagnostic suite

`harness/suite.py` runs a project's registered native diagnostics in one
unattended pass: one exclusive reservation, one private publication run per
check, a retained suite summary, and a reset between checks that leave the
guest in an uncertain state.  It owns the runner and the report contract
(ADR-0009); the **registry is consumer data**, and no Sherclawk diagnostic name
appears in harness code.  Sherclawk's registry is
[`tools/native-suite.json`](../../tools/native-suite.json).

```bash
./build.sh HarnessChecks_APPL
export OS9_HARNESS_SHARE="$HOME/Retro68Share"
export OS9_HARNESS_SHARE_HOST="<afp-host>"
python3 harness/suite.py validate --registry tools/native-suite.json --build build
python3 harness/suite.py run --registry tools/native-suite.json --build build --owner agent-a
```

`run` reserves a slot itself, so the caller does not need a token.  It keeps
the same lease while checks only read and write their private workspaces, and
releases and re-reserves the slot (a fresh overlay and a different boot ID)
after a check whose registry entry says `"reset": "reset"`.

## Registry

A registry is a JSON object with `"schema": 1` and a non-empty `checks` list.
Unknown keys are errors, so a typo cannot leave a check on a default.

```json
{
  "schema": 1,
  "checks": [
    {
      "name": "write",
      "target": "SherclawkWriteCheck",
      "timeout": 180,
      "expect": "passed",
      "reset": "keep",
      "mode": "automated",
      "fixtures": {"scenario": "fixtures/scenario.txt"},
      "description": "Create-only writes and exact bytes."
    }
  ]
}
```

| Field | Meaning |
|---|---|
| `name` | Unique lowercase slug used by `--check` and the summary. |
| `target` | A registered diagnostic in the harness contract; its build must exist before reserving. |
| `timeout` | Request deadline in guest seconds, `1..600`. The runner waits `timeout + 15` host seconds for the closed result. |
| `expect` | The completion outcome, `passed` (default) or `failed`. Only a closed, identity-matched report with this terminal outcome completes the check. |
| `reset` | `keep` (default) or `reset`; `reset` releases and re-reserves after the check. |
| `mode` | `automated` (default), `visual` or `model`. `run` refuses non-automated checks; see below. |
| `fixtures` | Optional `NAME -> relative-or-absolute-path` map published into the run workspace, at most 1 MiB each. |
| `description` | Optional text for humans. |

`validate` checks the registry without a guest, and with `--build` also checks
that every target's data fork and Rez resource fork exist and are non-empty.

## Completion rules

A check completes only when the controller's closed `result-<run>` arrives,
the harness contract validates its exact slot/generation/boot/run/diagnostic/
paths/deadline, and its terminal outcome equals `expect` and carries named
assertions.  These never pass any registry entry:

- a missing result (`timeout`/`unknown`),
- a malformed, partial or wrong-identity result,
- `harness_error`,
- `stopped` (Stop is an observation outcome, not cancellation or rollback),
- a process that disappears without a report.

An uncertain check is never retried or replayed.  `--keep-going` continues
only after the slot has been released and re-reserved; it never reuses the
guest that produced an uncertain result.  A timeout is reported as `unknown`
and its evidence is retained.  If a failed or uncertain check leaves an
unconsumed mailbox request, the suite stops and names the evidence instead of
deleting it or publishing a second request; recover the guest explicitly.
A release that quarantines its slot also fails the suite.

## Evidence

Each suite writes
`$OS9_HARNESS_HOME/evidence/suites/<suite>/`:

```
suite.json            summary, links and final result
session.json          live lease and current check (mode 0600)
interventions.jsonl   lease-checked screenshot/input/stop actions
```

Every check links its `publish.py` run directory under
`$OS9_HARNESS_HOME/evidence/<slot>/<slot>-g<gen>-run-<run>/`, which holds the
request, closed result, named assertions, screenshot, QEMU/controller/
diagnostic logs and source/artifact hashes.  Release and reset evidence stays
in the same per-slot evidence tree.  Nothing is deleted, including failed runs.

## Interactive checks

Visual assertions and live model probes do not belong in the timed,
deterministic suite.  Keep them in a separate manual pass while a suite is
running; the suite leaves `session.json` with the live token, owner, slot and
current request so these actions are still lease-checked:

```bash
python3 harness/suite.py screenshot --suite "$SUITE_DIR" --out shot.ppm
python3 harness/suite.py input --suite "$SUITE_DIR" --text "Hello" --click
python3 harness/suite.py stop --suite "$SUITE_DIR"
```

Each action revalidates the token/owner/generation and is appended to
`interventions.jsonl`.  `stop` publishes the matching `stop-<run>` record
once; a second Stop is refused rather than replayed.  A `visual` or `model`
registry entry is documentation for that manual pass, not work the unattended
runner will claim.

## Boundaries

- Build every selected application before reserving; `run` validates the
  registry, fixtures, targets, artifacts and the mounted share/share host up
  front, and reserves only after they are accepted.
- The suite publishes through `publish.py`, so it never restarts AFP and
  never changes the export.
- `SherclawkSelfBuildCheck` and `SherclawkSelfBuildStopCheck` are built with
  the private `buildjobs` queue name; their run workspace contains that queue,
  so they do not share `Worker01:buildjobs` with another guest.  Neither uses
  MacRelix: builds go through MPW ToolServer.
- One suite owns a slot at a time.  The reservation manager still permits a
  second suite on the other slot.
