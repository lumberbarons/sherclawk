# Running the guests: setup, a suite, inspection and recovery

This is the path an agent follows from nothing to released guests. The
reference pages it links to own the details; this page owns the order, the
places evidence lands and what to do when a step fails. The same workflow is
packaged for agents as [`skills/os9-guests/SKILL.md`](../skills/os9-guests/SKILL.md),
which lives under `harness/` so it moves with the harness
([ADR-0009](../../docs/adr/0009-build-harness-in-repo-then-extract.md)).

## Host setup, once per machine

1. Provision the slots ([provisioning.md](provisioning.md)): the frozen base,
   a controller-configured image per slot, and `provision.py info` listing
   every configured slot. Images, media and credentials stay under `$OS9_HARNESS_HOME`
   (default `~/.local/share/os9-harness`), never in a checkout.
2. Mount each slot's own `Retro68` volume writable on the host and record the
   mapping with `provision.py storage set` ([provisioning.md](provisioning.md#per-slot-afp-storage));
   `storage check` confirms every mount is its own slot's. AFP works where the host still has an AFP client. Otherwise export the
   slot's directory over NFS to this host only (`all_squash` to the share's owner,
   `insecure` for an unprivileged mount) and mount it with
   `mount_nfs -o vers=3,noresvport,nolocks,locallocks,noac,nonegnamecache`:
   readiness is a file replaced by rename, and with default attribute caching
   the host sees a stale heartbeat and refuses to publish. The harness never
   mounts, starts or restarts AFP.
3. Make the share host reachable over passwordless ssh with `sudo -n`
   ([publication.md](publication.md#configuration)).

```bash
export OS9_HARNESS_SHARE_HOST="<afp-host>"      # ssh target carrying the exports
```

QEMU must be on `PATH`. No API key or Sherclawk session is involved.

## Run the deterministic suite

Build the diagnostics first (`./build.sh HarnessChecks_APPL`; the Docker build
needs the user-supplied Universal Interfaces), then check what is running:

```bash
python3 harness/manage.py status          # capacity, free slots, owners, quarantine
python3 harness/suite.py validate --registry tools/native-suite.json --build build
python3 harness/suite.py run --registry tools/native-suite.json --build build \
    --owner "$USER-$(basename "$PWD")"
```

`suite.py run` reserves a slot, publishes one private workspace per check,
collects each closed result and **releases the guest itself**, including when a
check fails or the process raises. It exits `0` only when every check
completed as registered; a failed suite still leaves its evidence. Use one
stable owner name per agent: it is what `status` shows other agents.

If `status` shows no free slot, wait: a suite holds its slot for the whole
pass, and an idle reservation expires on its own after the lease lapses
([reservations.md](reservations.md#lease-rules)). Never take a slot by
recovering one that someone else owns.

## Reserve, work, release by hand

For anything that is not the registered suite (interactive checks, a one-off
diagnostic, looking at the desktop):

```bash
token=$(python3 harness/manage.py --json reserve --owner agent-a \
        | python3 -c 'import json,sys;print(json.load(sys.stdin)["token"])')
python3 harness/publish.py run --build build --target SherclawkWriteCheck \
    --token "$token" --owner agent-a --json
python3 harness/manage.py screenshot "$token" --owner agent-a --out shot.png
python3 harness/manage.py renew "$token" --owner agent-a     # long sessions
python3 harness/manage.py release "$token" --owner agent-a   # always, even after a failure
```

Keep the token out of shared logs. Release in a `trap` or a `finally` so a
crashed script does not hold a slot until expiry.

## Inspecting a live guest

Everything goes through the lease; the QMP socket is private to the manager.

- A held reservation: `manage.py screenshot` and `manage.py input`.
- Inside a running suite: `suite.py screenshot|input|stop --suite <dir>`. Each
  action revalidates the lease and is appended to `interventions.jsonl`.
  `--suite` is the directory `suite.py run` printed as its evidence.
- Pointer-heavy work (menus, drags) is [`guest.py`](driving.md) against a
  guest you own. The QMP socket serves one client at a time, so never start a
  second client while one process holds the connection. The display lags about
  one action, so wait a few seconds before a screenshot. A menu stays open
  only while its button is held (`phold`, `nudge`, screenshot, `release`) and
  must be driven from one process, since a new connection closes it.

## Where evidence lands

Nothing is deleted by the harness, including failed runs.

| What | Where |
|---|---|
| Suite summary, live session, interventions | `$OS9_HARNESS_HOME/evidence/suites/<suite>/` (`suite.json`, `session.json`, `interventions.jsonl`); `suite.py run` prints it |
| One run: request, closed result, assertions, screenshot, QEMU/controller/diagnostic logs, hashes | `$OS9_HARNESS_HOME/evidence/<slot>/<slot>-g<gen>-run-<run>/` |
| Release and recovery records with a last screenshot | `$OS9_HARNESS_HOME/evidence/<slot>/<slot>-g<gen>-<reason>-*.json` and `.ppm` |
| Guest-side mailbox, claims, results, boot traces | `Retro68:Harness:<slot>:` on the slot's own volume |
| Per-run workspaces and diagnostic logs | `Retro68:Harness:<slot>-g<gen>:<run>:` on the slot's own volume |
| Phase timings | `python3 harness/timing.py report <suite dir>...` (read-only) |

Disk images, credentials and these artifacts stay untracked. Record a
verification pass in [verification.md](verification.md) by path and hash, not
by copying evidence into git.

## Recovery

Start with `python3 harness/manage.py status --json`; it names the state and,
for a quarantine, the reason.

| Symptom | Do |
|---|---|
| A slot is `quarantined` | Read its reason, fix the cause (full disk, unwritable slot directory, an unidentifiable process), then `manage.py recover --slot slotN --reason "<what you fixed>"`. |
| A suite or script died holding a lease | Nothing, or `recover --slot slotN` once its lease expired. Expiry revokes the token before the slot is recovered. |
| The result never arrived (`unknown`) | Do not replay the request. Read the run evidence, screenshot and controller trace; the harness resets the guest on release. |
| A slot's controller never became ready | The manager removes stale `ready.tmp` staging and quarantines a failed boot. Check the QEMU log tail in the release evidence, then recover. |
| `publish` reports an unconsumed request | The previous run ended uncertain and left its `request` in `Retro68:Harness:<slot>:`. Recovery does not clear it yet (#260): keep the evidence, move that file out of the mailbox (for example into `Harness/archive/`) on the share, and never replay it. |
| App "in use" after republishing | A stale `afpd` session on the share host, covered in `AGENTS.md`. Kill only that process; never restart netatalk or the mount. |
| Every slot misbehaves after a host crash | `manage.py recover` with no `--slot` rotates the manager epoch, invalidates every lease and resets what it can. Only when no other agent holds a guest. |

Release stops the exact recorded QEMU pid with `SIGTERM` and then deletes and
recreates the slot's overlay, so an unclean guest shutdown costs nothing: the
next reservation boots a fresh overlay. For a guest you boot outside the
manager (preparing a base), use Special > Shut Down so OS 9 closes its volume;
that makes QEMU exit by itself.

## Throughput and timings

Run `harness/tests/guest_qualification.py` for a measured one-guest versus
two-guest comparison, or compare any finished suites yourself:

```bash
python3 harness/timing.py report "$SUITE_A" "$SUITE_B"   # suites that ran together
```

The report separates boot, publication, diagnostic execution plus result
collection, reset reboots and release, and gives checks per hour for the
combined wall time. Build time is not part of a suite; time `./build.sh`
separately. Treat the figures as properties of one host and one registry, and
keep the pool at its configured slots until a measurement says another would
pay for itself. Recorded measurements are in [verification.md](verification.md).
