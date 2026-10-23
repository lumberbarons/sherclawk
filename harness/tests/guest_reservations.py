#!/usr/bin/env python3
"""Opt-in guest acceptance for the two-slot reservation manager (#71).

Requires the provisioned OS 9 slots, QEMU, and the Retro68 export mounted on
the host (set ``OS9_HARNESS_SHARE`` or pass ``--share``). It boots both slots,
checks exclusive ownership and stale-token refusal, releases one slot and
reassigns it, then deliberately fails a reset to demonstrate quarantine and
explicit recovery. Evidence stays under ``$OS9_HARNESS_HOME/evidence``.

    OS9_HARNESS_SHARE="$HOME/Retro68Share" \
        python3 harness/tests/guest_reservations.py

This is not part of host discovery: it needs real guests and takes a few
minutes. Run it only when no agent holds a slot, because the initial clean
step invalidates leases and recovers both slots. It never restarts AFP and
never force-quits an unverified process.
"""
import argparse
import os
import stat
import sys
import tempfile
import threading

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import provision  # noqa: E402
import reservation  # noqa: E402


class AcceptanceError(Exception):
    pass


def check(condition, message):
    if not condition:
        raise AcceptanceError(message)
    print("PASS %s" % message)


def claim(manager_args, owner, barrier):
    manager = reservation.Manager(**manager_args)
    barrier.wait()
    return manager.reserve(owner)


def clean(manager):
    """Leave both slots free and stopped before the run starts."""
    manager.recover(reason="acceptance_reset")
    for slot in provision.SLOTS:
        state = next(s for s in manager.status()["slots"] if s["slot"] == slot)
        if state["state"] != "free":
            manager.recover(slot, reason="acceptance_reset")


def accept(args):
    home = provision.Home(args.home)
    manager_args = {"home": home, "share": args.share,
                    "ready_timeout": args.ready_timeout}
    manager = reservation.Manager(**manager_args)
    try:
        return _accept(manager, manager_args, args)
    finally:
        # Leave no guest running if an assertion fails; evidence stays.
        try:
            manager.recover(reason="acceptance_cleanup")
        except Exception:
            pass


def _accept(manager, manager_args, args):
    clean(manager)
    print("clean: both slots free")

    barrier = threading.Barrier(2)
    results = []

    def worker(owner):
        try:
            results.append(("ok", owner, claim(manager_args, owner, barrier)))
        except BaseException as exc:
            results.append(("error", owner, exc))

    threads = [threading.Thread(target=worker, args=(owner,))
               for owner in (args.owner_a, args.owner_b)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    failures = [item for item in results if item[0] == "error"]
    check(not failures, "concurrent reserves did not fail: %s" % failures)
    leases = {owner: lease for _kind, owner, lease in results if owner}
    a, b = leases[args.owner_a], leases[args.owner_b]
    check(a.slot != b.slot, "concurrent reserves hold distinct slots %s and %s"
          % (a.slot, b.slot))
    check(a.token != b.token, "concurrent reserves hold distinct random tokens")
    check(len(a.boot) == 32 and len(b.boot) == 32 and a.boot != b.boot,
          "each slot published its own fresh boot identity")
    check(manager.effects.process(a.pid).alive and manager.effects.process(b.pid).alive,
          "both QEMU processes are alive after readiness")
    try:
        manager.reserve("third-owner")
        raise AcceptanceError("a third reservation succeeded")
    except reservation.NoCapacity:
        print("PASS a third reservation is refused while both slots are held")

    shots = tempfile.mkdtemp(prefix="reservation-shots-")
    try:
        first = manager.screenshot(a.token, args.owner_a, os.path.join(shots, "slot-a.png"))
        second = manager.screenshot(b.token, args.owner_b, os.path.join(shots, "slot-b.png"))
        for shot in (first, second):
            check(os.path.getsize(shot["path"]) > 8, "captured %s" % shot["path"])
        check(first["path"] != second["path"], "the two slots captured distinct files")
    finally:
        for name in os.listdir(shots):
            os.remove(os.path.join(shots, name))
        os.rmdir(shots)

    released = manager.release(a.token, args.owner_a)
    check(released["stopped"] and released["reset"],
          "release stopped and reset %s" % a.slot)
    check(not manager.effects.process(a.pid).alive,
          "the released slot's exact QEMU pid is gone")
    check(bool(released["evidence"])
          and all(os.path.exists(path) for path in released["evidence"]),
          "release retained evidence at %s" % ", ".join(released["evidence"]))
    check(manager.release(a.token, args.owner_a)["already_released"],
          "releasing the same token again is a no-op")
    for name, call in (("renew", lambda: manager.renew(a.token, args.owner_a)),
                       ("screenshot", lambda: manager.screenshot(a.token, args.owner_a))):
        try:
            call()
            raise AcceptanceError("stale %s succeeded" % name)
        except reservation.LeaseError:
            print("PASS stale %s is refused after release" % name)

    c = manager.reserve(args.owner_c)
    check(c.slot == a.slot and c.generation > a.generation,
          "the freed slot was reassigned with generation %d" % c.generation)
    check(c.pid != a.pid and c.boot != a.boot,
          "the new owner has a different pid and fresh boot identity")
    check(manager.effects.process(c.pid).alive, "the new owner's QEMU is alive")
    try:
        manager.input(a.token, args.owner_a, text="stale")
        raise AcceptanceError("stale input succeeded after reassignment")
    except reservation.LeaseError:
        print("PASS stale input is refused after reassignment")
    check(manager.effects.process(c.pid).alive,
          "stale operations did not touch the new owner")

    if hasattr(os, "geteuid") and os.geteuid() == 0:
        print("SKIP quarantine: running as root, directory permissions cannot fail")
    else:
        slot_dir = os.path.dirname(provision.slot(manager.home, c.slot).overlay)
        mode = stat.S_IMODE(os.stat(slot_dir).st_mode)
        os.chmod(slot_dir, 0o500)
        try:
            broken = manager.release(c.token, args.owner_c)
        finally:
            os.chmod(slot_dir, mode)
        check(broken["quarantined"] and not broken["reset"],
              "a failed overlay reset quarantines %s" % c.slot)
        check(not manager.effects.process(c.pid).alive,
              "quarantine did not skip verified termination")
        status = next(s for s in manager.status()["slots"] if s["slot"] == c.slot)
        check(status["state"] == "quarantined" and status["quarantine"],
              "status reports the quarantine reason")
        recovered = manager.recover(c.slot, reason="acceptance_recovery")
        check(recovered["reset"], "explicit recovery reset the overlay")
        check(manager.status()["slots"][0 if c.slot == "slot1" else 1]["state"] == "free",
              "the recovered slot is assignable again")

    b_release = manager.release(b.token, args.owner_b)
    check(b_release["stopped"] and b_release["reset"], "released the second owner")
    manager.recover(reason="acceptance_final")
    status = manager.status()
    check(all(slot["state"] == "free" for slot in status["slots"]),
          "both slots are free and stopped at the end")
    print("ACCEPTED two-slot reservation manager; evidence under %s"
          % manager.evidence_dir)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--home", help="harness state directory (default $OS9_HARNESS_HOME)")
    p.add_argument("--share", help="mounted Retro68 export (default $OS9_HARNESS_SHARE)")
    p.add_argument("--ready-timeout", type=int, default=240)
    p.add_argument("--owner-a", default="acceptance-a")
    p.add_argument("--owner-b", default="acceptance-b")
    p.add_argument("--owner-c", default="acceptance-c")
    args = p.parse_args(argv)
    try:
        if not args.share and not os.environ.get("OS9_HARNESS_SHARE"):
            raise AcceptanceError("set OS9_HARNESS_SHARE to the mounted Retro68 export")
        accept(args)
        return 0
    except (AcceptanceError, reservation.ReservationError,
            provision.ProvisionError, OSError) as exc:
        print("FAIL %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
