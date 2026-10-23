#!/usr/bin/env python3
"""Run the registered native diagnostics as one unattended suite.

A registry owned by the consuming project lists the diagnostics, their request
deadline, expected terminal outcome and reset policy.  This module owns the
suite mechanics only: validation, one private publication run per check, an
optional fresh reservation between checks, a retained suite summary and the
lease-checked interactive escape hatches.  It knows no Sherclawk diagnostic
names beyond the versioned contract's registered allowlist.

    export OS9_HARNESS_SHARE="$HOME/Retro68Share"
    export OS9_HARNESS_SHARE_HOST="<afp-host>"
    python3 harness/suite.py run --registry tools/native-suite.json \
        --build build --owner agent-a

Every run writes ``$OS9_HARNESS_HOME/evidence/suites/<suite>/suite.json`` and
links the per-run evidence retained by ``publish.py``.  Nothing is deleted and
a failed run is never replayed.  Stdlib only; no imports from ``tools/`` or
``src/`` (ADR-0009).
"""
import argparse
import dataclasses
import hashlib
import json
import os
import re
import secrets
import sys
import time

import contract
import mailbox as harness_mailbox
import provision
import publish
import qmp
import reservation

SCHEMA = "1"
MODES = ("automated", "visual", "model")
EXPECTATIONS = ("passed", "failed")
RESET_POLICIES = ("keep", "reset")
CHECK_NAME = re.compile(r"[a-z0-9][a-z0-9_.-]{0,31}\Z")
CHECK_KEYS = {"name", "target", "timeout", "expect", "reset", "mode",
              "fixtures", "description"}
SESSION_NAME = "session.json"
SUMMARY_NAME = "suite.json"
INTERVENTIONS_NAME = "interventions.jsonl"
LEASE_RENEW_SECONDS = 60.0


class SuiteError(Exception):
    """The suite could not run or could not account for what it did."""


class RegistryError(SuiteError):
    """The consumer's registry is malformed or selects impossible work."""


class SessionError(SuiteError):
    """No active suite lease owns the requested interactive action."""


@dataclasses.dataclass(frozen=True)
class Check:
    """One registered diagnostic and its completion/reset policy."""

    name: str
    target: str
    timeout: int
    expect: str = "passed"
    reset: str = "keep"
    mode: str = "automated"
    fixtures: tuple = ()
    description: str = ""

    def record(self):
        return {
            "name": self.name,
            "target": self.target,
            "timeout": self.timeout,
            "expect": self.expect,
            "reset": self.reset,
            "mode": self.mode,
            "fixtures": [name for name, _ in self.fixtures],
            "description": self.description,
        }


@dataclasses.dataclass(frozen=True)
class Registry:
    """A validated registry file and its exact bytes."""

    path: str
    digest: str
    checks: tuple

    def record(self):
        return {"path": self.path, "sha256": self.digest,
                "checks": len(self.checks)}

    def select(self, names=None):
        if not names:
            return list(self.checks)
        wanted = set(names)
        known = {check.name for check in self.checks}
        unknown = sorted(wanted - known)
        if unknown:
            raise RegistryError("unknown check(s) %s; registry has %s"
                                % (", ".join(unknown), ", ".join(sorted(known))))
        return [check for check in self.checks if check.name in wanted]


def load_registry(path):
    """Read and strictly validate a registry before anything is reserved.

    The file is consumer data.  Unknown keys are errors so a typo cannot
    silently leave a check on a default; all registered targets are still
    checked against the harness contract, never a hard-coded project list.
    """
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError as exc:
        raise RegistryError("cannot read registry %s: %s" % (path, exc))
    try:
        data = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError) as exc:
        raise RegistryError("registry %s is not valid UTF-8 JSON: %s" % (path, exc))
    if not isinstance(data, dict):
        raise RegistryError("registry %s must be a JSON object" % path)
    if data.get("schema") not in (1, "1"):
        raise RegistryError("registry %s has schema %r, expected 1"
                            % (path, data.get("schema")))
    entries = data.get("checks")
    if not isinstance(entries, list) or not entries:
        raise RegistryError("registry %s needs a non-empty checks list" % path)
    checks, names = [], set()
    for entry in entries:
        if not isinstance(entry, dict):
            raise RegistryError("registry %s checks entries must be objects" % path)
        unknown = sorted(set(entry) - CHECK_KEYS)
        if unknown:
            raise RegistryError("registry %s check has unknown key(s) %s"
                                % (path, ", ".join(unknown)))
        name = entry.get("name")
        if not isinstance(name, str) or not CHECK_NAME.match(name):
            raise RegistryError("registry %s check name %r is not a slug"
                                % (path, name))
        if name in names:
            raise RegistryError("registry %s repeats check %r" % (path, name))
        names.add(name)
        target = entry.get("target")
        if not isinstance(target, str) or target not in contract.DIAGNOSTICS:
            raise RegistryError("registry %s check %r target %r is not registered"
                                % (path, name, target))
        timeout = entry.get("timeout")
        if isinstance(timeout, bool) or not isinstance(timeout, int) or not 1 <= timeout <= 600:
            raise RegistryError("registry %s check %r timeout must be 1..600"
                                % (path, name))
        expect = entry.get("expect", "passed")
        if expect not in EXPECTATIONS:
            raise RegistryError("registry %s check %r expect must be one of %s"
                                % (path, name, ", ".join(EXPECTATIONS)))
        reset = entry.get("reset", "keep")
        if reset not in RESET_POLICIES:
            raise RegistryError("registry %s check %r reset must be one of %s"
                                % (path, name, ", ".join(RESET_POLICIES)))
        mode = entry.get("mode", "automated")
        if mode not in MODES:
            raise RegistryError("registry %s check %r mode must be one of %s"
                                % (path, name, ", ".join(MODES)))
        raw_fixtures = entry.get("fixtures", {})
        if not isinstance(raw_fixtures, dict):
            raise RegistryError("registry %s check %r fixtures must be an object"
                                % (path, name))
        fixtures = []
        for fixture, source in raw_fixtures.items():
            problem = publish.fixture_problem(fixture, target)
            if problem:
                raise RegistryError("registry %s check %r: %s"
                                    % (path, name, problem))
            if not isinstance(source, str) or not source:
                raise RegistryError("registry %s check %r fixture %r needs a path"
                                    % (path, name, fixture))
            source = source if os.path.isabs(source) else os.path.join(
                os.path.dirname(os.path.abspath(path)), source)
            fixtures.append((fixture, os.path.abspath(source)))
        description = entry.get("description", "")
        if not isinstance(description, str):
            raise RegistryError("registry %s check %r description must be text"
                                % (path, name))
        checks.append(Check(name, target, timeout, expect, reset, mode,
                            tuple(fixtures), description))
    return Registry(os.path.abspath(path), hashlib.sha256(raw).hexdigest(),
                    tuple(checks))


def utc(wall):
    return time.strftime("%Y%m%dT%H%M%SZ", time.gmtime(wall))


def _stamp(clock):
    wall = clock.wall()
    return {"monotonic": clock.monotonic(), "wall": wall, "utc": utc(wall)}


def intervention_line(suite, clock, action, fields, error=None):
    entry = {"schema": SCHEMA, "suite": suite, "at": _stamp(clock),
             "action": action, "error": error}
    entry.update(fields)
    return json.dumps(entry, sort_keys=True) + "\n"


def append_intervention(directory, suite, clock, action, fields, error=None):
    """Append one interactive action to a suite directory from another process."""
    path = os.path.join(directory, INTERVENTIONS_NAME)
    line = intervention_line(suite, clock, action, fields, error)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_APPEND, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        f.write(line)
    return line


class LeaseKeeper:
    """Renew a suite lease while a bounded check is waiting.

    A self-build can occupy most of a 600-second request deadline, so waiting
    without renewal would let the lease lapse and make the result unattributable.
    """

    def __init__(self, manager, lease, interval=LEASE_RENEW_SECONDS):
        self.manager = manager
        self.lease = lease
        self.interval = max(1.0, float(interval))
        self.last = manager.clock.monotonic()

    def tick(self):
        now = self.manager.clock.monotonic()
        remaining = self.lease.expires_in(now, self.manager.clock.wall())
        if now - self.last < self.interval and remaining > 120:
            return
        self.lease = self.manager.renew(self.lease.token, self.lease.owner)
        self.last = now

    def replace(self, lease):
        self.lease = lease
        self.last = self.manager.clock.monotonic()


class Suite:
    """Publish and collect registered checks in registry order under one lease."""

    def __init__(self, home=None, share=None, share_host=None, share_dir=None,
                 clock=None, manager=None, publisher=None, evidence_dir=None,
                 screenshot=True, suite_id=None):
        self.clock = clock or reservation.Clock()
        self.home = home if isinstance(home, provision.Home) else provision.Home(home)
        share = share if share is not None else os.environ.get("OS9_HARNESS_SHARE")
        share_host = share_host or os.environ.get("OS9_HARNESS_SHARE_HOST")
        self.manager = manager or reservation.Manager(home=self.home, share=share,
                                                      clock=self.clock)
        self.publisher = publisher or publish.Publisher(
            home=self.home, share=share, share_host=share_host,
            share_dir=share_dir, manager=self.manager, clock=self.clock)
        self.evidence_root = evidence_dir or os.path.join(
            self.home.path, "evidence", "suites")
        self.screenshot = screenshot
        self.suite_id = suite_id
        self.summary = None
        self.session = None
        self._session_path = None
        self._interventions = None

    # ---- preflight -------------------------------------------------------

    def _artifacts(self, checks, build, supplied):
        prepared = {}
        for check in checks:
            artifact = (supplied or {}).get(check.target)
            if artifact is None:
                if not build:
                    raise RegistryError(
                        "check %r needs --build or a supplied artifact" % check.name)
                artifact = publish.locate_artifact(build, check.target)
            if artifact.target != check.target:
                raise RegistryError("artifact for %r names %r"
                                    % (check.target, artifact.target))
            prepared[check.target] = artifact
        return prepared

    @staticmethod
    def _fixtures(checks):
        prepared = {}
        for check in checks:
            for name, source in check.fixtures:
                try:
                    with open(source, "rb") as f:
                        data = f.read(publish.FIXTURE_MAX + 1)
                except OSError as exc:
                    raise RegistryError("check %r fixture %s: %s"
                                        % (check.name, source, exc))
                problem = publish.fixture_problem(name, check.target, data)
                if problem:
                    raise RegistryError("check %r: %s" % (check.name, problem))
                prepared[(check.name, name)] = data
        return prepared

    # ---- evidence and session -------------------------------------------

    def _new_directory(self):
        suite_id = self.suite_id or "%s-%s" % (utc(self.clock.wall()),
                                               secrets.token_hex(8))
        directory = os.path.join(self.evidence_root, suite_id)
        try:
            os.makedirs(directory, exist_ok=False)
        except FileExistsError:
            raise SuiteError("suite evidence directory already exists: %s"
                             % directory)
        self._session_path = os.path.join(directory, SESSION_NAME)
        self._interventions = os.path.join(directory, INTERVENTIONS_NAME)
        fd = os.open(self._interventions, os.O_WRONLY | os.O_CREAT | os.O_EXCL,
                     0o600)
        os.close(fd)
        return suite_id, directory

    def _write_json(self, path, data, mode=0o644):
        temporary = path + ".tmp"
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL, mode)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2, sort_keys=True)
                f.write("\n")
        except BaseException:
            try:
                os.unlink(temporary)
            except OSError:
                pass
            raise
        os.replace(temporary, path)

    def _write_summary(self):
        self.summary["updated"] = _stamp(self.clock)
        self._write_json(os.path.join(self.summary["evidence_dir"], SUMMARY_NAME),
                         self.summary)

    def _session(self, **fields):
        self.session.update(fields)
        self.session["updated"] = _stamp(self.clock)
        self._write_json(self._session_path, self.session, mode=0o600)

    # ---- suite -----------------------------------------------------------

    def run(self, registry, build=None, owner=None, selected=None,
            keep_going=False, artifacts=None):
        """Run selected checks serially; never retry an uncertain check."""
        if not owner:
            raise SuiteError("the suite needs a non-empty owner name")
        checks = registry.select(selected)
        manual = [check.name for check in checks if check.mode != "automated"]
        if manual:
            raise RegistryError(
                "check(s) %s are not automated; run visual/model checks "
                "interactively with suite.py screenshot/input/stop"
                % ", ".join(manual))
        prepared = self._artifacts(checks, build, artifacts)
        fixtures = self._fixtures(checks)
        require_ready = getattr(self.publisher, "require_ready", None)
        if require_ready is not None:
            require_ready()

        suite_id, directory = self._new_directory()
        self.summary = {
            "schema": SCHEMA,
            "suite": suite_id,
            "owner": owner,
            "registry": registry.record(),
            "build": os.path.abspath(build) if build else None,
            "phase": "running",
            "error": None,
            "passed": False,
            "stopped_early": False,
            "started": _stamp(self.clock),
            "finished": None,
            "checks": [],
            "reservations": [],
            "releases": [],
            "current": None,
            "evidence_dir": directory,
            "session": self._session_path,
            "interventions": self._interventions,
        }
        self.session = {"schema": SCHEMA, "active": False, "suite": suite_id,
                        "owner": owner}
        self._session()
        self._write_summary()

        lease = None
        keeper = None
        try:
            lease = self.manager.reserve(owner)
            self._reserved(lease, "suite start")
            keeper = LeaseKeeper(self.manager, lease)
            self._session(active=True, owner=owner, token=lease.token,
                          slot=lease.slot, generation=lease.generation,
                          boot=lease.boot)
            for index, check in enumerate(checks):
                keeper.tick()
                self.summary["current"] = {
                    "name": check.name, "target": check.target,
                    "started": _stamp(self.clock)}
                self._write_summary()
                step = self._run_check(check, lease, prepared[check.target],
                                       fixtures, keeper)
                self.summary["current"] = None
                self.summary["checks"].append(step)
                self._write_summary()
                remaining = checks[index + 1:]
                failed = not step["passed"]
                mailbox = (step.get("run") or {}).get("mailbox")
                if mailbox and os.path.exists(os.path.join(mailbox, "request")):
                    self.summary["error"] = (
                        "check %r left an unconsumed request; recover the guest "
                        "before another check (evidence: %s)"
                        % (check.name, step["run"]["evidence_dir"]))
                    self.summary["stopped_early"] = True
                    break
                if failed and not keep_going:
                    self.summary["stopped_early"] = bool(remaining)
                    break
                if remaining:
                    if check.reset == "reset" or failed:
                        current, lease = lease, None
                        self._release(current, owner,
                                      "reset after %s" % check.name)
                        lease = self.manager.reserve(owner)
                        self._reserved(lease, "fresh after %s" % check.name)
                        keeper.replace(lease)
                        self._session(active=True, token=lease.token,
                                      owner=owner, slot=lease.slot,
                                      generation=lease.generation,
                                      boot=lease.boot, check=None,
                                      request=None)
            self.summary["passed"] = (
                bool(self.summary["checks"])
                and not self.summary["stopped_early"]
                and len(self.summary["checks"]) == len(checks)
                and all(step["passed"] for step in self.summary["checks"])
                and self.summary["error"] is None)
            self.summary["phase"] = "finished" if self.summary["passed"] else "failed"
        except Exception as exc:
            self.summary["phase"] = "failed"
            self.summary["error"] = "%s: %s" % (type(exc).__name__, str(exc)[:500])
            raise
        finally:
            if lease is not None:
                current, lease = lease, None
                try:
                    self._release(current, owner, "suite finished")
                except Exception as exc:
                    self.summary["error"] = self.summary["error"] or (
                        "release: %s" % str(exc)[:500])
                    self.summary["passed"] = False
                    self.summary["phase"] = "failed"
            self.summary["finished"] = _stamp(self.clock)
            self._session(active=False, token=None, request=None, check=None)
            self._write_summary()
        return self.summary

    def _run_check(self, check, lease, artifact, fixtures, keeper):
        started = _stamp(self.clock)
        entry = {
            "name": check.name, "target": check.target, "mode": check.mode,
            "timeout": check.timeout, "expect": check.expect,
            "reset": check.reset, "started": started, "finished": None,
            "outcome": "error", "passed": False, "assertions": 0,
            "failures": 0, "failed_assertions": [], "error": None, "run": None,
        }
        run = self.publisher.begin(lease.token, lease.owner, artifact)
        entry["run"] = {
            "id": run.run_id,
            "slot": lease.slot,
            "generation": lease.generation,
            "workspace": run.workspace,
            "application": run.application,
            "evidence_dir": run.evidence_dir,
            "evidence": getattr(run, "evidence_path", None),
            "result": None,
            "mailbox": run.mailbox_share,
        }
        self._session(active=True, check={
            "name": check.name, "target": check.target, "run_id": run.run_id,
            "workspace": run.workspace, "evidence_dir": run.evidence_dir},
            mailbox=run.mailbox_share)
        try:
            run.publish_application()
            for name, source in check.fixtures:
                run.publish_fixture(name, fixtures[(check.name, name)])
            run.publish_request(check.timeout)
            entry["run"]["result"] = run.result_path()
            self._session(active=True, request={
                "path": os.path.join(run.evidence_dir, "request.bin"),
                "run": run.request.run,
                "deadline": run.request.deadline})
            report = run.collect(timeout=check.timeout + publish.WAIT_GRACE,
                                 screenshot=self.screenshot, on_poll=keeper.tick)
            entry["outcome"] = report.outcome
            entry["assertions"] = len(report.checks)
            entry["failures"] = sum(not ok for ok, _ in report.checks)
            entry["failed_assertions"] = [name for ok, name in report.checks
                                          if not ok][:20]
            entry["passed"] = report.outcome == check.expect
            entry["report"] = {"outcome": report.outcome,
                               "assertions": entry["assertions"],
                               "failures": entry["failures"]}
        except publish.RunTimeout as exc:
            entry["outcome"] = "unknown"
            entry["error"] = str(exc)[:500]
        except reservation.LeaseError as exc:
            entry["outcome"] = "lease_lost"
            entry["error"] = str(exc)[:500]
        except contract.ContractError as exc:
            entry["outcome"] = "invalid_evidence"
            entry["error"] = str(exc)[:500]
        except Exception as exc:
            entry["outcome"] = "error"
            entry["error"] = "%s: %s" % (type(exc).__name__, str(exc)[:500])
        entry["finished"] = _stamp(self.clock)
        entry["seconds"] = round(entry["finished"]["monotonic"]
                                 - entry["started"]["monotonic"], 3)
        if entry["run"].get("evidence") and os.path.exists(entry["run"]["evidence"]):
            with open(entry["run"]["evidence"], encoding="utf-8") as f:
                try:
                    entry["run"]["phase"] = json.load(f).get("phase")
                except ValueError:
                    entry["run"]["phase"] = None
        fields = {
            "check": {
                "name": check.name, "target": check.target,
                "run_id": run.run_id, "workspace": run.workspace,
                "evidence_dir": run.evidence_dir,
                "outcome": entry["outcome"]},
        }
        if "report" in entry:
            # A closed report ends the run; do not offer Stop for it.
            fields["request"] = None
        self._session(active=True, **fields)
        return entry

    def _reserved(self, lease, reason):
        self.summary["reservations"].append({
            "slot": lease.slot, "generation": lease.generation,
            "boot": lease.boot, "at": _stamp(self.clock), "reason": reason})
        self._write_summary()

    def _release(self, lease, owner, reason):
        entry = {"slot": lease.slot, "generation": lease.generation,
                 "reason": reason, "at": _stamp(self.clock)}
        try:
            entry["result"] = self.manager.release(lease.token, owner)
        except Exception as exc:
            entry["error"] = "%s: %s" % (type(exc).__name__, str(exc)[:500])
            self.summary["releases"].append(entry)
            self._write_summary()
            raise
        self.summary["releases"].append(entry)
        self._write_summary()
        if entry["result"].get("quarantined"):
            raise SuiteError("slot %s quarantined during %s: %s"
                             % (lease.slot, reason,
                                entry["result"].get("error", "recovery failed")))
        return entry


# ---- interactive actions under the live suite lease -----------------------

def read_session(directory):
    path = directory
    if os.path.isdir(directory):
        path = os.path.join(directory, SESSION_NAME)
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError) as exc:
        raise SessionError("cannot read suite session %s: %s" % (path, exc))
    if not isinstance(data, dict) or data.get("schema") != SCHEMA:
        raise SessionError("%s is not a suite session" % path)
    return data


def active_session(directory):
    data = read_session(directory)
    if not data.get("active") or not data.get("token"):
        raise SessionError("suite %s is not holding a lease"
                           % data.get("suite", "?"))
    if not data.get("owner") or not data.get("check"):
        raise SessionError("suite %s has no current check" % data.get("suite", "?"))
    return data


def _intervention(directory, data, action, fields, error=None):
    suite_dir = directory if os.path.isdir(directory) else os.path.dirname(
        os.path.abspath(directory))
    append_intervention(suite_dir, data.get("suite", "?"), reservation.Clock(),
                        action, fields, error)
    return error


def screenshot(directory, manager, out):
    data = active_session(directory)
    try:
        result = manager.screenshot(data["token"], data["owner"], out)
    except Exception as exc:
        _intervention(directory, data, "screenshot", {"out": out}, str(exc)[:500])
        raise
    _intervention(directory, data, "screenshot", {"out": result.get("path")})
    return result


def input_text(directory, manager, text=None, keys=(), click=False):
    data = active_session(directory)
    try:
        result = manager.input(data["token"], data["owner"], text, list(keys), click)
    except Exception as exc:
        _intervention(directory, data, "input", {"text": text, "keys": list(keys),
                                                 "click": click}, str(exc)[:500])
        raise
    _intervention(directory, data, "input", {"typed": result.get("typed"),
                                             "keys": result.get("keys"),
                                             "clicked": result.get("clicked")})
    return result


def stop(directory, manager):
    """Publish Stop for the current run, once, after revalidating the lease."""
    data = active_session(directory)
    lease = manager.lease(data["token"], data["owner"])
    request_path = (data.get("request") or {}).get("path")
    mailbox = data.get("mailbox")
    if not request_path or not mailbox:
        raise SessionError("suite %s has no published request to stop"
                           % data.get("suite", "?"))
    try:
        with open(request_path, "rb") as f:
            request = contract.read_request(f.read(contract.REQUEST_CAP))
    except (OSError, contract.ContractError) as exc:
        raise SessionError("cannot read the current request: %s" % exc)
    if (request.slot != lease.slot or request.boot != lease.boot
            or request.generation != "%032x" % lease.generation):
        raise SessionError("current request does not belong to the live lease")
    path = os.path.join(mailbox, contract.filename(request, "stop"))
    try:
        harness_mailbox.publish(path, contract.write_request(request))
    except FileExistsError:
        raise SessionError("Stop was already published for this run; not replaying")
    _intervention(directory, data, "stop", {"request": request_path, "stop": path})
    return {"stop": path, "run": request.run}


# ---- command line ---------------------------------------------------------

def build_parser():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--home", help="harness state directory (default $OS9_HARNESS_HOME)")
    ap.add_argument("--share", help="mounted Retro68 export (default $OS9_HARNESS_SHARE)")
    ap.add_argument("--share-host", help="AFP share host (default $OS9_HARNESS_SHARE_HOST)")
    ap.add_argument("--share-dir", help="server-side export (default %s)"
                    % publish.DEFAULT_SHARE_DIR)
    ap.add_argument("--json", action="store_true", help="structured output")
    common = argparse.ArgumentParser(add_help=False)
    common.add_argument("--home", default=argparse.SUPPRESS)
    common.add_argument("--share", default=argparse.SUPPRESS)
    common.add_argument("--share-host", default=argparse.SUPPRESS)
    common.add_argument("--share-dir", default=argparse.SUPPRESS)
    common.add_argument("--json", action="store_true", default=argparse.SUPPRESS)
    sub = ap.add_subparsers(dest="command", required=True)

    def command(name, **kw):
        kw.setdefault("parents", [common])
        return sub.add_parser(name, **kw)

    p = command("run", help="run registered checks serially")
    p.add_argument("--registry", required=True)
    p.add_argument("--build", required=True)
    p.add_argument("--owner", required=True)
    p.add_argument("--check", action="append", default=[],
                   help="run only this registered check (repeatable)")
    p.add_argument("--keep-going", action="store_true",
                   help="reset after a failure and run the remaining checks")
    p.add_argument("--no-screenshot", action="store_true")

    p = command("validate", help="validate a registry, and optionally its artifacts")
    p.add_argument("--registry", required=True)
    p.add_argument("--build")

    p = command("screenshot", help="capture the guest under the live suite lease")
    p.add_argument("--suite", required=True, help="suite evidence directory")
    p.add_argument("--out")

    p = command("input", help="type, press keys or click under the live suite lease")
    p.add_argument("--suite", required=True, help="suite evidence directory")
    p.add_argument("--text")
    p.add_argument("--key", action="append", default=[])
    p.add_argument("--click", action="store_true")

    p = command("stop", help="publish Stop for the current run")
    p.add_argument("--suite", required=True, help="suite evidence directory")
    return ap


def emit(args, result):
    if args.json:
        json.dump(result, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
        return
    if args.command == "validate":
        print("registry %s checks=%d" % (result["path"], len(result["checks"])))
        for check in result["checks"]:
            print("%-12s %-28s timeout=%3d expect=%-6s reset=%s mode=%s"
                  % (check["name"], check["target"], check["timeout"],
                     check["expect"], check["reset"], check["mode"]))
        return
    if args.command == "run":
        print("suite %s owner=%s checks=%d"
              % (result["suite"], result["owner"], len(result["checks"])))
        for check in result["checks"]:
            print("%-12s %-28s %-16s assertions=%d failures=%d evidence=%s"
                  % (check["name"], check["target"], check["outcome"],
                     check["assertions"], check["failures"],
                     check["run"]["evidence_dir"] if check["run"] else "-"))
        print("suite %s %d/%d"
              % ("passed" if result["passed"] else "failed",
                 sum(step["passed"] for step in result["checks"]),
                 len(result["checks"])))
        print("evidence %s" % result["evidence_dir"])
        return
    print(json.dumps(result, indent=2, sort_keys=True))


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        if args.command == "validate":
            registry = load_registry(args.registry)
            result = registry.record()
            result["checks"] = [check.record() for check in registry.checks]
            if args.build:
                for check in registry.checks:
                    artifact = publish.locate_artifact(args.build, check.target)
                    next(entry for entry in result["checks"]
                         if entry["name"] == check.name)["artifact"] = artifact.record()
            emit(args, result)
            return 0
        if args.command == "run":
            registry = load_registry(args.registry)
            runner = Suite(home=args.home, share=args.share,
                           share_host=args.share_host, share_dir=args.share_dir,
                           screenshot=not args.no_screenshot)
            result = runner.run(registry, args.build, args.owner,
                                selected=args.check or None,
                                keep_going=args.keep_going)
            emit(args, result)
            if result["passed"]:
                return 0
            print("suite: %s" % (result["error"] or "one or more checks failed"),
                  file=sys.stderr)
            print("suite: evidence retained under %s" % result["evidence_dir"],
                  file=sys.stderr)
            return 1
        manager = reservation.Manager(home=args.home, share=args.share)
        if args.command == "screenshot":
            emit(args, screenshot(args.suite, manager, args.out))
        elif args.command == "input":
            emit(args, input_text(args.suite, manager, args.text, args.key,
                                  args.click))
        else:
            emit(args, stop(args.suite, manager))
        return 0
    except (RegistryError, publish.ArtifactError, publish.UsageError,
            reservation.UsageError, SessionError) as exc:
        print("suite: %s" % exc, file=sys.stderr)
        return 2
    except (SuiteError, reservation.LeaseError, reservation.NoCapacity,
            publish.ConfigError, reservation.ConfigError) as exc:
        print("suite: %s" % exc, file=sys.stderr)
        return 2
    except (publish.PublicationError, reservation.ReservationError,
            reservation.RecoveryError, provision.ProvisionError,
            contract.ContractError, qmp.QMPError, qmp.KeymapError,
            OSError, ValueError) as exc:
        print("suite: %s" % exc, file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
