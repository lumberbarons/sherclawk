#!/usr/bin/env python3
"""Reserve and drive one of the two OS 9 QEMU slots.

One manager per host keeps its state outside every worktree, in
``$OS9_HARNESS_HOME``. A reservation returns a random lease token and a
generation; every later command checks both, under an OS lock, before touching
the guest. Readiness is read from the mounted ``Retro68`` export, so set
``OS9_HARNESS_SHARE`` (or pass ``--share``) before reserving.

    export OS9_HARNESS_SHARE="$HOME/Retro68Share"
    token=$(manage.py --json reserve --owner agent-a | python3 -c 'import json,sys;print(json.load(sys.stdin)["token"])')
    manage.py screenshot "$token" --owner agent-a --out shot.png
    manage.py release "$token" --owner agent-a

Exit codes: 0 success, 1 a failed ``suite`` step, 2 stale lease/expired/
capacity/usage, 3 guest, host or recovery failure. ``run`` returns the child's
own status, so 2 and 3 are ambiguous there; a manager error is the one that
prints a ``manage:`` line on stderr. Stdlib only; no imports from tools/ or
src/ (ADR-0009).
"""
import argparse
import json
import shlex
import sys

import contract
import provision
import qmp
import reservation


def build_parser():
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n")[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="\n".join(__doc__.split("\n")[1:]))
    ap.add_argument("--home", help="harness state directory (default $OS9_HARNESS_HOME)")
    ap.add_argument("--share", help="mounted Retro68 export (default $OS9_HARNESS_SHARE)")
    ap.add_argument("--json", action="store_true", help="structured output")
    # The same flags after a subcommand use SUPPRESS defaults so they win over
    # the root defaults without clobbering values given before it.
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--home", default=argparse.SUPPRESS)
    common.add_argument("--share", default=argparse.SUPPRESS)
    common.add_argument("--json", action="store_true", default=argparse.SUPPRESS)
    sub = ap.add_subparsers(dest="command", required=True)

    def command(name, **kw):
        kw.setdefault("parents", [common])
        return sub.add_parser(name, **kw)

    p = command("reserve", help="claim a free slot and boot it")
    p.add_argument("--owner", required=True)
    p.add_argument("--ttl", type=int, default=reservation.LEASE_SECONDS,
                   help="lease seconds, 1..600 (default 600)")

    p = command("renew", help="extend an active lease")
    p.add_argument("token")
    p.add_argument("--owner", required=True)
    p.add_argument("--ttl", type=int, default=reservation.LEASE_SECONDS)

    p = command("run", help="run a bounded host command under a lease")
    p.add_argument("token")
    p.add_argument("--owner", required=True)
    p.add_argument("--timeout", type=int, default=300)

    p = command("suite", help="run bounded commands in sequence under a lease")
    p.add_argument("token")
    p.add_argument("--owner", required=True)
    p.add_argument("--timeout", type=int, default=300, help="seconds per step")
    p.add_argument("--keep-going", action="store_true")
    p.add_argument("--step", action="append", default=[],
                   help="NAME::COMMAND (repeatable; COMMAND is shell-split)")

    p = command("screenshot", help="capture the reserved guest")
    p.add_argument("token")
    p.add_argument("--owner", required=True)
    p.add_argument("--out", help="PNG or PPM path")

    p = command("input", help="type text, press keys or click")
    p.add_argument("token")
    p.add_argument("--owner", required=True)
    p.add_argument("--text")
    p.add_argument("--key", action="append", default=[])
    p.add_argument("--click", action="store_true")

    p = command("release", help="revoke, stop, verify, reset and free the slot")
    p.add_argument("token")
    p.add_argument("--owner", required=True)

    command("status", help="show manager, lease and guest state")

    p = command("recover", help="recover one slot or every slot")
    p.add_argument("--slot", choices=("slot1", "slot2"))
    p.add_argument("--reason", default="manual")
    return ap


def parse_steps(raw_steps):
    steps = []
    for raw in raw_steps:
        if "::" not in raw:
            raise reservation.UsageError("--step must be NAME::COMMAND")
        name, command = raw.split("::", 1)
        try:
            argv = shlex.split(command)
        except ValueError as exc:
            raise reservation.UsageError("--step command: %s" % exc)
        if not name.strip() or not argv:
            raise reservation.UsageError("--step needs a name and a command")
        steps.append((name.strip(), argv))
    if not steps:
        raise reservation.UsageError("suite needs at least one --step")
    return steps


def emit(args, result):
    if args.json:
        json.dump(result, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
        return
    if args.command == "status":
        manager = result["manager"]
        print("manager epoch=%s host_boot=%s free=%d/%d lease=%ss" % (
            manager["epoch"], manager["host_boot"], manager["free"],
            manager["capacity"], manager["lease_seconds"]))
        for slot in result["slots"]:
            if slot["state"] == "free":
                print("%s free generation=%d" % (slot["slot"], slot["generation"]))
                continue
            print("%s %s generation=%d owner=%s expires_in=%ss pid=%s boot=%s%s" % (
                slot["slot"], slot["state"], slot["generation"], slot["owner"],
                slot["expires_in"], slot["pid"], slot["boot"],
                " quarantine=%s" % slot["quarantine"] if slot["quarantine"] else ""))
    elif args.command in ("reserve", "renew"):
        print("%s %s generation=%d owner=%s expires_in=%ss" % (
            args.command, result["slot"], result["generation"], result["owner"],
            result["expires_in"]))
        print("token %s" % result["token"])
    elif args.command == "screenshot":
        print(result["path"])
    elif args.command in ("release", "recover"):
        print(json.dumps(result, indent=2, sort_keys=True))
    elif args.command == "input":
        print("typed=%d keys=%d clicked=%s" % (
            result["typed"], result["keys"], result["clicked"]))
    elif args.command == "suite":
        for step in result["steps"]:
            outcome = ("ok" if step["returncode"] == 0 else
                       "timeout" if step.get("timeout") else "exit %d" % step["returncode"])
            print("%-20s %s" % (step["name"], outcome))
        print("suite %s" % ("passed" if result["passed"] else "failed"))


def main(argv=None):
    raw = list(sys.argv[1:] if argv is None else argv)
    parser = build_parser()
    args, extra = parser.parse_known_args(raw)
    if args.command != "run" and extra:
        parser.error("unrecognized arguments: %s" % " ".join(extra))
    if args.command == "run":
        command = list(extra)
        if command and command[0] == "--":
            command = command[1:]
    try:
        manager = reservation.Manager(home=args.home, share=args.share)
        if args.command == "reserve":
            lease = manager.reserve(args.owner, args.ttl)
            emit(args, lease.public(manager.clock.monotonic(), manager.clock.wall(),
                                     manager.clock.source))
        elif args.command == "renew":
            lease = manager.renew(args.token, args.owner, args.ttl)
            emit(args, lease.public(manager.clock.monotonic(), manager.clock.wall(),
                                     manager.clock.source))
        elif args.command == "run":
            code = manager.run(args.token, args.owner, command, args.timeout)
            if code < 0:
                code = 1
            return min(code, 255)
        elif args.command == "suite":
            steps = parse_steps(args.step)
            result = manager.suite(args.token, args.owner,
                                   [(name, command, args.timeout) for name, command in steps],
                                   not args.keep_going)
            emit(args, result)
            return 0 if result["passed"] else 1
        elif args.command == "screenshot":
            emit(args, manager.screenshot(args.token, args.owner, args.out))
        elif args.command == "input":
            emit(args, manager.input(args.token, args.owner, args.text, args.key, args.click))
        elif args.command == "release":
            emit(args, manager.release(args.token, args.owner))
        elif args.command == "status":
            emit(args, manager.status())
        elif args.command == "recover":
            emit(args, manager.recover(args.slot, args.reason))
    except reservation.LeaseError as exc:
        print("manage: %s" % exc, file=sys.stderr)
        return 2
    except (reservation.NoCapacity, reservation.ConfigError, reservation.UsageError,
            qmp.KeymapError) as exc:
        print("manage: %s" % exc, file=sys.stderr)
        return 2
    except reservation.RecoveryError as exc:
        print("manage: %s" % exc, file=sys.stderr)
        return 3
    except (reservation.ReservationError, provision.ProvisionError,
            contract.ContractError, qmp.QMPError, OSError, ValueError) as exc:
        print("manage: %s" % exc, file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
