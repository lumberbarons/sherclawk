#!/usr/bin/env python3
"""Opt-in two-guest qualification: isolation, a deliberate failure and throughput (#74).

Runs the registered suite alone on one slot, then two agents' suites at the
same time on both slots, and prints the phase timings and checks per hour of
each so the one-versus-two guest comparison comes from measurements. While the
two suites run it checks that a third reservation is refused and that a live
lease refuses both a made-up token and its real token under the other owner's
name. The registry it runs is a copy of the consumer's with a
``deliberate-failure`` check appended
(``HarnessAcceptance`` told to fail, expected to fail), so every run also shows
a failing assertion being recorded without disturbing the other guest.

    ./build.sh HarnessChecks_APPL
    OS9_HARNESS_SHARE="$HOME/Retro68Share" OS9_HARNESS_SHARE_HOST=<afp-host> \
        python3 harness/tests/guest_qualification.py --build build

Not part of host discovery: it needs two provisioned guests, the mounted
share and several minutes per pass. Run it only when no agent holds a slot,
because the initial clean step invalidates leases and recovers both slots.
Evidence stays under ``$OS9_HARNESS_HOME/evidence``; the timing reports are
also written to ``--out``. It never restarts AFP or force-quits a process.
"""
import argparse
import json
import os
import secrets
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
import provision  # noqa: E402
import reservation  # noqa: E402
import timing  # noqa: E402

REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_REGISTRY = os.path.join(REPO, "tools", "native-suite.json")


class QualificationError(Exception):
    pass


def note(message):
    print(message, flush=True)


def check(condition, message):
    if not condition:
        raise QualificationError(message)
    note("PASS %s" % message)


def write_registry(args, scratch):
    """Copy the consumer registry's chosen checks and append the failure check."""
    with open(args.registry, encoding="utf-8") as f:
        registry = json.load(f)
    base = os.path.dirname(os.path.abspath(args.registry))
    wanted = [name for name in args.checks.split(",") if name]
    known = {entry["name"]: entry for entry in registry["checks"]}
    missing = [name for name in wanted if name not in known]
    if missing:
        raise QualificationError("registry has no check named %s" % ", ".join(missing))
    checks = []
    for name in wanted:
        entry = dict(known[name])
        entry["fixtures"] = {key: os.path.join(base, value) if not os.path.isabs(value)
                             else value
                             for key, value in entry.get("fixtures", {}).items()}
        if not entry["fixtures"]:
            del entry["fixtures"]
        checks.append(entry)
    scenario = os.path.join(scratch, "scenario-fail")
    with open(scenario, "w", encoding="ascii") as f:
        f.write("fail")
    checks.append({"name": "deliberate-failure", "target": "HarnessAcceptance",
                   "timeout": 60, "expect": "failed", "reset": "keep",
                   "mode": "automated", "fixtures": {"scenario": scenario},
                   "description": "Qualification: a recorded assertion failure."})
    path = os.path.join(scratch, "registry.json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump({"schema": registry["schema"], "checks": checks}, f, indent=2)
    return path


class Suite:
    """One `suite.py run` child; output goes to files so neither pipe can fill."""

    def __init__(self, args, registry, owner, scratch):
        self.owner = owner
        argv = [sys.executable, os.path.join(REPO, "harness", "suite.py"), "--json",
                "run", "--registry", registry, "--build", args.build, "--owner", owner]
        if args.home:
            argv[3:3] = ["--home", args.home]
        if args.share:
            argv[3:3] = ["--share", args.share]
        self.out_path = os.path.join(scratch, owner + ".out")
        self.err_path = os.path.join(scratch, owner + ".err")
        with open(self.out_path, "w") as out, open(self.err_path, "w") as err:
            self.process = subprocess.Popen(argv, stdout=out, stderr=err, text=True)

    def stop(self):
        """Leave no child behind when the qualification fails; evidence stays."""
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=30)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()

    def finish(self):
        self.process.wait()
        with open(self.out_path, encoding="utf-8") as f:
            out = f.read()
        try:
            summary = json.loads(out)
        except ValueError:
            with open(self.err_path, encoding="utf-8") as f:
                err = f.read()
            raise QualificationError("suite for %s produced no summary (exit %s): %s"
                                     % (self.owner, self.process.returncode, err.strip()[-400:]))
        check(summary["passed"], "%s: every check completed as expected "
              "(%s)" % (self.owner, ", ".join(step["name"] for step in summary["checks"])))
        return summary


def solo(args, registry, scratch):
    note("== one agent, one guest")
    started = time.monotonic()
    child = Suite(args, registry, args.owner_a, scratch)
    try:
        summary = child.finish()
    finally:
        child.stop()
    note("solo wall %.1fs" % (time.monotonic() - started))
    return summary


def live_leases(manager, owners, deadline, stop):
    """Wait until every owner holds a booted lease; return their slot rows."""
    while time.monotonic() < deadline and not stop.is_set():
        held = {slot["owner"]: slot for slot in manager.status()["slots"]
                if slot["owner"] in owners and slot["state"] == "active"}
        if len(held) == len(owners):
            return held
        time.sleep(2)
    raise QualificationError("both agents did not reach a live lease in time")


def session_token(home, owner):
    """The live token a suite advertises in its private session file."""
    root = os.path.join(home.path, "evidence", "suites")
    for name in sorted(os.listdir(root), reverse=True):
        try:
            with open(os.path.join(root, name, "session.json"), encoding="utf-8") as f:
                data = json.load(f)
        except (OSError, ValueError):
            continue
        if data.get("owner") == owner and data.get("active") and data.get("token"):
            return data["token"]
    raise QualificationError("no live session for %s" % owner)


def isolation_probe(manager, args, stop):
    held = live_leases(manager, {args.owner_a, args.owner_b},
                       time.monotonic() + args.ready_timeout + 120, stop)
    check(held[args.owner_a]["slot"] != held[args.owner_b]["slot"],
          "the two agents hold distinct slots %s and %s"
          % (held[args.owner_a]["slot"], held[args.owner_b]["slot"]))
    check(held[args.owner_a]["boot"] != held[args.owner_b]["boot"],
          "the two guests booted with distinct identities")
    try:
        extra = manager.reserve(args.owner_c)
    except reservation.NoCapacity:
        note("PASS a third reservation is refused while both agents hold a slot")
    else:
        # One suite finished between the check above and this call; give the
        # guest back before reporting that the probe lost its race.
        manager.release(extra.token, args.owner_c)
        raise QualificationError("a third reservation succeeded; a suite may have "
                                 "finished before the probe ran, so rerun")
    real = session_token(manager.home, args.owner_a)
    for name, call in (
            ("a made-up token", lambda: manager.screenshot(secrets.token_hex(16),
                                                           args.owner_a)),
            ("a live token under the other owner's name",
             lambda: manager.screenshot(real, args.owner_b))):
        try:
            call()
        except reservation.LeaseError:
            note("PASS %s is refused against a live lease" % name)
        else:
            raise QualificationError("%s was accepted" % name)


def pair(args, registry, manager, scratch):
    note("== two agents, two guests at once")
    started = time.monotonic()
    children = {owner: Suite(args, registry, owner, scratch)
                for owner in (args.owner_a, args.owner_b)}
    probe = {}
    stop = threading.Event()

    def run_probe():
        try:
            isolation_probe(manager, args, stop)
        except BaseException as exc:
            probe["error"] = exc

    thread = threading.Thread(target=run_probe, daemon=True)
    thread.start()
    try:
        summaries = {owner: child.finish() for owner, child in children.items()}
    finally:
        stop.set()
        for child in children.values():
            child.stop()
        thread.join(timeout=30)
    if "error" in probe:
        raise probe["error"]
    note("pair wall %.1fs" % (time.monotonic() - started))
    runs = [step["run"] for summary in summaries.values() for step in summary["checks"]]
    workspaces = [run["workspace"] for run in runs]
    check(len(workspaces) == len(set(workspaces)),
          "every check in both suites ran in its own never-reused workspace")
    slots = {owner: {step["run"]["slot"] for step in summary["checks"]}
             for owner, summary in summaries.items()}
    check(all(len(s) == 1 for s in slots.values())
          and slots[args.owner_a] != slots[args.owner_b],
          "each suite stayed on its own slot (%s)" % slots)
    return summaries


def accept(args):
    home = provision.Home(args.home)
    manager = reservation.Manager(home=home, share=args.share,
                                  ready_timeout=args.ready_timeout)
    manager.recover(reason="qualification_reset")
    try:
        with tempfile.TemporaryDirectory(prefix="qualification-") as scratch:
            registry = write_registry(args, scratch)
            one = solo(args, registry, scratch)
            both = pair(args, registry, manager, scratch)
    finally:
        # A failed pass must not leave a booted, leased guest for the next one.
        try:
            manager.recover(reason="qualification_final")
        except reservation.ReservationError as exc:
            print("WARN final recovery failed: %s" % exc, file=sys.stderr)
    check(all(s["state"] == "free" for s in manager.status()["slots"]),
          "both slots are free and stopped at the end")
    reports = {"one_guest": timing.report([one["evidence_dir"]]),
               "two_guests": timing.report([s["evidence_dir"] for s in both.values()])}
    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, "qualification-%s.json" % time.strftime("%Y%m%dT%H%M%S"))
    with open(path, "w", encoding="utf-8") as f:
        json.dump(reports, f, indent=2, sort_keys=True)
    for label, report in reports.items():
        note("-- %s" % label)
        note(timing.render(report))
    note("ACCEPTED two-guest qualification; timings in %s" % path)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--home", help="harness state directory (default $OS9_HARNESS_HOME)")
    p.add_argument("--share", help="mounted Retro68 export (default $OS9_HARNESS_SHARE)")
    p.add_argument("--build", required=True, help="build tree from ./build.sh HarnessChecks_APPL")
    p.add_argument("--registry", default=DEFAULT_REGISTRY)
    p.add_argument("--checks", default="write,inspect",
                   help="comma-separated registry checks to run besides the deliberate failure")
    p.add_argument("--out", default=os.path.join(REPO, "build", "qualification"),
                   help="directory for the timing JSON (default build/, untracked)")
    p.add_argument("--ready-timeout", type=int, default=240)
    p.add_argument("--owner-a", default="qualify-a")
    p.add_argument("--owner-b", default="qualify-b")
    p.add_argument("--owner-c", default="qualify-c")
    args = p.parse_args(argv)
    args.share = args.share or os.environ.get("OS9_HARNESS_SHARE")
    try:
        if not args.share or not os.environ.get("OS9_HARNESS_SHARE_HOST"):
            raise QualificationError(
                "set OS9_HARNESS_SHARE and OS9_HARNESS_SHARE_HOST for publication")
        accept(args)
        return 0
    except (QualificationError, timing.TimingError, reservation.ReservationError,
            provision.ProvisionError, OSError) as exc:
        print("FAIL %s" % exc, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
