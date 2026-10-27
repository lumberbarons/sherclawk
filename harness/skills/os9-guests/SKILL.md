---
name: os9-guests
description: Use the shared QEMU Mac OS 9 guests to run native diagnostics or look at a running guest. Use when asked how many guests there are or what they are doing, to run the native suite, to reserve, drive, screenshot or release a guest, or to recover a stuck or quarantined one.
---

# OS 9 guests

Other agents share the same guests. Reserve before touching one, release when
done, and never reach a guest except through the commands below. Run
everything from the repository root; the full reference is
`harness/docs/workflow.md`.

## What is running

```bash
python3 harness/manage.py status          # add --json for fields
```

The first line is the manager: capacity, how many slots are free and the lease
length. Then one line per slot: `free`, or `active`/`launching`/`recovering`/
`expired`/`quarantined` with the owner, seconds until the lease expires, pid and
boot ID, and the quarantine reason. That answers how many guests exist and who
holds them. For what a held guest is doing, list the active suites and their current check.
`OS9_HARNESS_HOME` is usually unset; the default is used below. The script
prints no token, and `session.json` holds a live one, so never `cat` it.

```bash
H="${OS9_HARNESS_HOME:-$HOME/.local/share/os9-harness}"
python3 - "$H" <<'EOF'
import glob, json, sys
for path in sorted(glob.glob(sys.argv[1] + "/evidence/suites/*/session.json")):
    s = json.load(open(path))
    if s.get("active"):
        print(s["owner"], s.get("slot"), (s.get("check") or {}).get("name"), path)
EOF
```

## Run the registered suite

```bash
export OS9_HARNESS_SHARE_HOST=<afp-host>
./build.sh HarnessChecks_APPL                       # build first; needs INTERFACES_DIR
python3 harness/suite.py run --registry tools/native-suite.json --build build \
    --owner <your-stable-name>
```

It reserves a slot, runs each check in its own workspace and releases the guest
itself. Exit `0` means every check completed as registered. If no slot is free,
wait and retry; do not recover someone else's slot. The result is in
`$H/evidence/suites/<suite>/suite.json`; time it with
`python3 harness/timing.py report <suite dir>`.

## Reserve and drive a guest by hand

```bash
token=$(python3 harness/manage.py --json reserve --owner <name> \
        | python3 -c 'import json,sys;print(json.load(sys.stdin)["token"])')
python3 harness/manage.py screenshot "$token" --owner <name> --out shot.png
python3 harness/manage.py input "$token" --owner <name> --text "Hello" --click
python3 harness/manage.py renew "$token" --owner <name>      # leases lapse when idle
python3 harness/manage.py release "$token" --owner <name>    # always, also after errors
```

Release in a `trap`/`finally`. Keep the token out of shared output. For a
check already running under `suite.py`, use `suite.py screenshot|input|stop
--suite <dir>` instead; those reuse the suite's lease and are logged.

## Driving rules that cost time when ignored

- **One QMP client at a time.** The socket serves a single connection. Never
  start `guest.py` (or a second manager command) from a process that already
  holds it; a wedged call is a bounded error, not a hang, but it still loses
  the action.
- **The display lags one action.** Wait a few seconds after an action before a
  screenshot, or you will read the previous screen.
- **Menus need the button held.** `phold` on the menu title, `nudge` down (about
  28 per item), screenshot to confirm, then `release`. Keep that whole sequence
  in one process: a new connection re-locates the pointer and closes the menu.
  Moves under about 15 are ignored; keys held under 200 ms are dropped.
- **Use closed-loop pointer commands** (`pgoto`, `pclick`, `phold`, `nudge`) from
  `harness/guest.py`; blind `moveto` is for menus you will verify afterwards.
  `harness/docs/driving.md` lists the commands.
- **Clean shutdown.** Releasing a slot sends `SIGTERM` to the exact recorded
  QEMU pid and then recreates its overlay, so an unclean guest costs nothing.
  Only a guest you boot yourself (preparing a base) needs Special > Shut Down.
  Never `kill -9` a QEMU pid and never restart AFP or netatalk.

## Recovery

```bash
python3 harness/manage.py status --json                      # state and quarantine reason
python3 harness/manage.py recover --slot slotN --reason "<what you fixed>"
```

- `quarantined`: fix the stated cause first (disk space, an unwritable slot
  directory, an unidentifiable process), then `recover --slot`.
- Lease expired or the owner died: `recover --slot` after expiry; the token is
  already revoked.
- Result `unknown` or a request left unconsumed: never replay it. Read the run
  evidence under `$H/evidence/<slot>/`, then recover the slot.
- `recover` with no `--slot` invalidates every lease. Use it only when no other
  agent holds a guest.
- Republished app reports "in use": a stale `afpd` session on the share host.
  Kill only that process (see `AGENTS.md`).

Images, credentials and evidence stay under `$H` (the harness home) and out of git.
