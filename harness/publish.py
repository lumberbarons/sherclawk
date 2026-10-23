#!/usr/bin/env python3
"""Lease-aware publication and evidence for reserved OS 9 diagnostic runs.

Sherclawk supplies the built artifact (data fork plus Rez resource fork) and
the registered target name; this module owns staging, fork metadata,
publication and collection, under a reservation lease:

    export OS9_HARNESS_SHARE="$HOME/Retro68Share"
    export OS9_HARNESS_SHARE_HOST="<afp-host>"
    token=$(python3 harness/manage.py --json reserve --owner agent-a \
            | python3 -c 'import json,sys;print(json.load(sys.stdin)["token"])')
    python3 harness/publish.py run --build build --target SherclawkWriteCheck \
        --token "$token" --owner agent-a --fixture scenario=scenario.txt

Each run publishes into a fresh, never-reused workspace
``Retro68:Harness:<slot>-g<generation>:<run>:` on the existing export, with
the application, its ``._`` sidecar and the complete netatalk Metadata xattr
verified server-side before the mailbox request is published last.  The
workspace also holds the run's private ``buildjobs`` queue.  Evidence is
retained under ``$OS9_HARNESS_HOME/evidence/<slot>/<slot>-g<gen>-run-<run>/``
and survives failed runs, expiry and repeated launches; nothing is deleted.

Stdlib only, and no imports from ``tools/`` or ``src/`` (ADR-0009).  Never
restarts AFP or changes the export.
"""
import argparse
import contextlib
import dataclasses
import json
import os
import re
import secrets
import shlex
import subprocess
import sys
import tempfile

import contract
import mailbox as harness_mailbox
import netatalk_meta
import provision
import qmp
import reservation

SCHEMA = "1"
DEFAULT_SHARE_DIR = "/srv/retro68"
HARNESS_ROOT = "Retro68:Harness:"
QUEUE_NAME = "buildjobs"
STAGE_PREFIX = "sherclawk-harness-publish."
STAGE_LEAF = re.compile(r"[A-Za-z0-9._-]{1,64}\Z")
REQUEST_SECONDS = 120
WAIT_GRACE = 15.0
POLL_SECONDS = 0.25
FIXTURE_MAX = 1 << 20
LOG_TAIL_BYTES = 8192


class PublicationError(Exception):
    """A run could not be published or collected as asked."""


class ArtifactError(PublicationError):
    """The selected target's build output is missing, empty or not registered."""


class RemoteError(PublicationError):
    """A share-host staging, installation or verification step failed."""


class ConfigError(PublicationError):
    """The publisher is not configured for the share or the share host."""


class UsageError(PublicationError):
    """The request itself is malformed: a bad argument, range or name."""


class RunTimeout(PublicationError):
    """No closed result arrived within the bounded wait."""


@dataclasses.dataclass(frozen=True)
class Artifact:
    """A validated local build output for one registered diagnostic target."""

    target: str
    build_dir: str
    data: str
    resource: str
    data_sha256: str
    resource_sha256: str
    data_bytes: int
    resource_bytes: int

    def record(self):
        return {
            "target": self.target,
            "build_dir": self.build_dir,
            "data": self.data,
            "resource": self.resource,
            "data_sha256": self.data_sha256,
            "resource_sha256": self.resource_sha256,
            "data_bytes": self.data_bytes,
            "resource_bytes": self.resource_bytes,
        }


def locate_artifact(build_dir, target, diagnostics=contract.DIAGNOSTICS, registered=True):
    """Derive the registered target's forks from the build tree, or refuse.

    The path mapping is exact, so a request can never be published for a
    different application than the one selected; missing or empty forks are
    rejected before any guest is reserved. ``registered=False`` allows
    installing a built application that is not a harness diagnostic.
    """
    if registered and target not in diagnostics:
        raise ArtifactError("target %r is not a registered diagnostic (%s)"
                            % (target, ", ".join(diagnostics)))
    build_dir = os.path.abspath(build_dir)
    data = os.path.join(build_dir, target + ".APPL")
    resource = os.path.join(build_dir, ".rsrc", target + ".APPL")
    for path, fork in ((data, "data"), (resource, "resource")):
        if not os.path.isfile(path):
            raise ArtifactError("no %s fork for %s at %s" % (fork, target, path))
        if os.path.getsize(path) == 0:
            raise ArtifactError("empty %s fork for %s at %s" % (fork, target, path))
    return Artifact(target, build_dir, data, resource,
                    netatalk_meta.sha256_file(data), netatalk_meta.sha256_file(resource),
                    os.path.getsize(data), os.path.getsize(resource))


def source_state(repo_dir):
    """The revision and dirty state that produced the artifact, best effort."""
    state = {"repo": repo_dir, "revision": None, "dirty": None, "error": None}
    try:
        revision = subprocess.run(["git", "-C", repo_dir, "rev-parse", "HEAD"],
                                  capture_output=True, text=True, timeout=10)
        if revision.returncode:
            raise OSError((revision.stderr or revision.stdout).strip() or "not a git checkout")
        status = subprocess.run(["git", "-C", repo_dir, "status", "--porcelain",
                                 "--untracked-files=no"],
                                capture_output=True, text=True, timeout=10)
        if status.returncode:
            raise OSError((status.stderr or status.stdout).strip())
        state["revision"] = revision.stdout.strip()
        state["dirty"] = bool(status.stdout.strip())
    except (OSError, subprocess.SubprocessError) as exc:
        state["error"] = str(exc)[:200]
    return state


def tail_bytes(path, cap=LOG_TAIL_BYTES):
    """A bounded tail of a log file, or None when it cannot be read."""
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - cap))
            return f.read(cap)
    except OSError:
        return None


class Remote:
    """The share-host boundary: bounded ssh/scp staging and verification.

    One instance stages into one never-reused remote scratch directory; the
    harvest of netatalk metadata runs server-side because the xattr it must
    set does not exist in a macOS mount.
    """

    def __init__(self, host, share_dir=DEFAULT_SHARE_DIR, user="macos9",
                 group="macos9", python="python3", timeout=120.0):
        if not host:
            raise ConfigError("set OS9_HARNESS_SHARE_HOST to the AFP server hostname")
        self.host = host
        self.share_dir = share_dir or DEFAULT_SHARE_DIR
        self.user = user
        self.group = group
        self.python = python
        self.timeout = float(timeout)
        self._stage = None
        self._script = None

    def _capture(self, argv, check=True):
        try:
            done = subprocess.run(argv, capture_output=True, text=True,
                                  timeout=self.timeout)
        except subprocess.TimeoutExpired:
            raise RemoteError("%s timed out after %.0f seconds" % (argv[0], self.timeout))
        except OSError as exc:
            raise RemoteError("%s could not run: %s" % (argv[0], exc))
        if check and done.returncode:
            detail = (done.stderr or done.stdout or "").strip()
            raise RemoteError("%s failed (exit %d): %s"
                              % (argv[0], done.returncode, detail[-300:]))
        return done

    def ssh(self, script, check=True):
        return self._capture(["ssh", "-o", "BatchMode=yes", self.host, script],
                             check=check).stdout

    def scp(self, local, remote_path):
        self._capture(["scp", "-q", "-o", "BatchMode=yes", local,
                       "%s:%s" % (self.host, remote_path)])

    def staging(self):
        if self._stage is None:
            output = self.ssh("mktemp -d /tmp/%sXXXXXX" % STAGE_PREFIX).strip()
            if not output.startswith("/tmp/%s" % STAGE_PREFIX) \
                    or not STAGE_LEAF.match(output.rsplit("/", 1)[-1]):
                raise RemoteError("unexpected staging path %r" % output[:120])
            self._stage = output
        return self._stage

    def upload(self, local, leaf):
        path = "%s/%s" % (self.staging(), self._leaf(leaf))
        self.scp(local, path)
        return path

    def stage_bytes(self, leaf, data):
        handle, local = tempfile.mkstemp(prefix="harness-publish-")
        try:
            with os.fdopen(handle, "wb") as f:
                f.write(data)
            return self.upload(local, leaf)
        finally:
            with contextlib.suppress(OSError):
                os.unlink(local)

    def _leaf(self, leaf):
        if not STAGE_LEAF.match(leaf or ""):
            raise RemoteError("invalid staging leaf %r" % (leaf,))
        return leaf

    def make_dir(self, path):
        self.ssh("sudo -n install -d -o %s -g %s -m 0755 %s"
                 % (self.user, self.group, shlex.quote(path)))

    def install(self, staged, dest):
        self.ssh("sudo -n install -o %s -g %s -m 0666 %s %s"
                 % (self.user, self.group, shlex.quote(staged), shlex.quote(dest)))

    def meta_script(self):
        if self._script is None:
            local = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "netatalk_meta.py")
            self._script = self.upload(local, "netatalk_meta.py")
        return self._script

    def set_metadata(self, dest):
        self.ssh("sudo -n %s %s xattr %s"
                 % (self.python, shlex.quote(self.meta_script()), shlex.quote(dest)))

    def inspect(self, dest):
        script = "sudo -n %s %s check %s" % (self.python,
                                             shlex.quote(self.meta_script()),
                                             shlex.quote(dest))
        done = self._capture(["ssh", "-o", "BatchMode=yes", self.host, script], check=False)
        try:
            payload = json.loads(done.stdout.strip())
        except ValueError:
            detail = (done.stderr or done.stdout or "").strip()
            raise RemoteError("check produced no JSON: %s" % detail[-300:])
        if not isinstance(payload, dict) or "state" not in payload:
            raise RemoteError("check produced an unexpected payload")
        return payload["state"]

    def cleanup(self):
        stage, self._stage = self._stage, None
        if stage:
            with contextlib.suppress(PublicationError):
                self.ssh("rm -rf %s" % shlex.quote(stage), check=False)


def publish_artifact(remote, artifact, dest, leaf_prefix="publish"):
    """Stage, install and verify one artifact at an exact remote path.

    Verifies the data fork, the resource-fork sidecar and the complete
    netatalk Metadata record server-side; any mismatch raises.
    """
    with open(artifact.data, "rb") as f:
        data = f.read()
    sidecar = netatalk_meta.sidecar_bytes(artifact.build_dir, artifact.target)
    staged_data = remote.stage_bytes("%s-data" % leaf_prefix, data)
    staged_sidecar = remote.stage_bytes("%s-sidecar" % leaf_prefix, sidecar)
    remote.install(staged_data, dest)
    remote.install(staged_sidecar, os.path.join(os.path.dirname(dest),
                                                "._" + os.path.basename(dest)))
    remote.set_metadata(dest)
    state = remote.inspect(dest)
    found = netatalk_meta.problems(state,
                                   expect_data_sha256=artifact.data_sha256,
                                   expect_resource_sha256=artifact.resource_sha256)
    if found:
        raise PublicationError("published application failed verification at %s: %s"
                               % (dest, "; ".join(found)))
    return state


def valid_fixture_name(name):
    if not isinstance(name, str) or not name or len(name) > 31 or name in (".", "..") \
            or name.startswith("."):
        return False
    try:
        raw = name.encode("ascii")
    except UnicodeEncodeError:
        return False
    return all(32 <= byte <= 126 for byte in raw) and ":" not in name


class Publisher:
    """Lease-aware publication into private run workspaces on the export."""

    def __init__(self, home=None, share=None, share_host=None, share_dir=None,
                 clock=None, remote=None, manager=None, evidence_dir=None):
        self.clock = clock or reservation.Clock()
        self.home = home if isinstance(home, provision.Home) else provision.Home(home)
        share = share if share is not None else os.environ.get("OS9_HARNESS_SHARE")
        self.share = os.path.abspath(share) if share else None
        self.share_host = share_host or os.environ.get("OS9_HARNESS_SHARE_HOST")
        self.share_dir = (share_dir or os.environ.get("OS9_HARNESS_SHARE_DIR")
                          or DEFAULT_SHARE_DIR)
        if remote is not None:
            self.remote = remote
        elif self.share_host:
            self.remote = Remote(self.share_host, self.share_dir)
        else:
            self.remote = None
        self.manager = manager or reservation.Manager(home=self.home, share=self.share,
                                                      clock=self.clock)
        self.evidence_dir = evidence_dir or os.path.join(self.home.path, "evidence")

    def require_ready(self):
        if not self.share:
            raise ConfigError("set OS9_HARNESS_SHARE to the mounted Retro68 export")
        if not os.path.isdir(self.share):
            raise ConfigError("share is not mounted: %s" % self.share)
        if self.remote is None:
            raise ConfigError("set OS9_HARNESS_SHARE_HOST to publish to the share host")

    def mount_path(self, guest):
        return self._guest_path(self.share, guest)

    def remote_path(self, guest):
        return self._guest_path(self.share_dir, guest)

    @staticmethod
    def _guest_path(root, guest):
        parts = str(guest).rstrip(":").split(":")
        if len(parts) < 2 or parts[0] != "Retro68" or any(not part for part in parts):
            raise PublicationError("not a Retro68 guest path: %r" % (guest,))
        return os.path.join(root, *parts[1:])

    def begin(self, token, owner, artifact):
        """Validate the lease and open a fresh, never-reused run."""
        self.require_ready()
        lease = self.manager.lease(token, owner)
        run = Run(self, lease, artifact, secrets.token_hex(16))
        run.start()
        return run

    def cleanup(self):
        if self.remote is not None:
            self.remote.cleanup()


class Run:
    """One diagnostic run: private paths, publication steps and evidence."""

    def __init__(self, publisher, lease, artifact, run_id):
        self.publisher = publisher
        self.lease = lease
        self.artifact = artifact
        self.run_id = run_id
        self.short = run_id[:20]
        self.reservation = "%s-g%d" % (lease.slot, lease.generation)
        self.workspace = "%s%s:%s:" % (HARNESS_ROOT, self.reservation, self.short)
        self.application = self.workspace + artifact.target
        self.queue = self.workspace + QUEUE_NAME + ":"
        self.mailbox_guest = "%s%s:" % (HARNESS_ROOT, lease.slot)
        self.mailbox_share = publisher.mount_path(self.mailbox_guest)
        self.evidence_dir = os.path.join(
            publisher.evidence_dir, lease.slot,
            "%s-g%d-run-%s" % (lease.slot, lease.generation, self.short))
        self.evidence_path = os.path.join(self.evidence_dir, "evidence.json")
        self.fixtures = []
        self.steps = []
        self.state = "begun"
        self.error = None
        self.request = None
        self.report = None
        self.seconds = None
        self.verification = None
        self.captures = {}
        self.logs = {}
        self.source = None
        self.emulator = None
        self.manager_info = None
        self.started_mono = publisher.clock.monotonic()
        self.started_wall = publisher.clock.wall()

    @property
    def workspace_mount(self):
        return self.publisher.mount_path(self.workspace)

    def start(self):
        os.makedirs(self.evidence_dir, exist_ok=True)
        self.source = source_state(os.path.dirname(self.artifact.build_dir))
        self.emulator = self._emulator_config()
        try:
            self.manager_info = self.publisher.manager.status()["manager"]
        except reservation.ReservationError:
            self.manager_info = {"epoch": None, "host_boot": None}
        self._write_evidence()

    def _emulator_config(self):
        slot = provision.slot(self.publisher.home, self.lease.slot)
        config = {"machine": "mac99", "argv": provision.qemu_argv(slot),
                  "mac": slot.mac, "overlay": slot.overlay, "qmp": slot.qmp}
        try:
            config["qemu"] = provision.qemu_version()
        except (provision.ProvisionError, OSError, ValueError) as exc:
            config["qemu_error"] = str(exc)[:200]
        return config

    # ---- publication -----------------------------------------------------

    def _require_lease(self):
        return self.publisher.manager.lease(self.lease.token, self.lease.owner)

    def _step(self, name, function, require_lease=True):
        entry = {"name": name, "started": self.publisher.clock.monotonic(),
                 "status": "running"}
        self.steps.append(entry)
        try:
            if require_lease:
                self._require_lease()
            value = function()
        except BaseException as exc:
            entry["status"] = "failed"
            entry["detail"] = str(exc)[:400]
            entry["finished"] = self.publisher.clock.monotonic()
            self.error = str(exc)
            self.state = "failed"
            self._write_evidence()
            raise
        entry["status"] = "ok"
        entry["finished"] = self.publisher.clock.monotonic()
        self._write_evidence()
        return value

    @property
    def application_published(self):
        return any(step["name"] == "application" and step["status"] == "ok"
                   for step in self.steps)

    def publish_application(self):
        """Publish the exact artifact into the run workspace and verify it."""
        def body():
            remote = self.publisher.remote
            remote.make_dir(self.publisher.remote_path(self.workspace))
            remote.make_dir(self.publisher.remote_path(self.queue))
            self.verification = publish_artifact(
                remote, self.artifact, self.publisher.remote_path(self.application),
                leaf_prefix=self.short)
        return self._step("application", body)

    def publish_fixture(self, name, data):
        """Publish one fixture file into the run workspace."""
        if not valid_fixture_name(name):
            raise UsageError("fixture name %r is not a single 1..31 byte HFS name" % (name,))
        if not isinstance(data, (bytes, bytearray)):
            raise UsageError("fixture %s must be bytes" % name)
        data = bytes(data)
        if len(data) > FIXTURE_MAX:
            raise UsageError("fixture %s is larger than %d bytes" % (name, FIXTURE_MAX))

        def body():
            remote = self.publisher.remote
            staged = remote.stage_bytes("fixture-%s-%s" % (self.short, name), data)
            dest = self.publisher.remote_path(self.workspace + name)
            remote.install(staged, dest)
            self.fixtures.append({"name": name, "bytes": len(data),
                                  "sha256": netatalk_meta.sha256(data),
                                  "guest": self.workspace + name, "remote": dest})
        return self._step("fixture:" + name, body)

    def publish_request(self, seconds=REQUEST_SECONDS):
        """Publish the mailbox request last, after every artifact is verified."""
        if not 1 <= seconds <= 600:
            raise UsageError("request seconds must be within 1..600")
        if self.request is not None:
            raise PublicationError("this run's request is already published")
        if not self.application_published:
            raise PublicationError("publish and verify the application before the request")

        def body():
            request_path = os.path.join(self.mailbox_share, "request")
            if os.path.exists(request_path):
                raise PublicationError(
                    "mailbox %s already holds an unconsumed request; recover the guest "
                    "before a new run" % self.mailbox_guest)
            with contextlib.suppress(OSError):
                os.remove(request_path + ".tmp")  # a dead publish's staging, not evidence
            with open(os.path.join(self.mailbox_share, "ready"), "rb") as f:
                boot, clock = contract.read_ready_clock(f.read(129), self.lease.slot)
            if boot != self.lease.boot:
                raise PublicationError(
                    "guest boot changed from %s to %s; re-reserve the slot"
                    % (self.lease.boot, boot))
            request = contract.Request(self.lease.slot,
                                       "%032x" % self.lease.generation, boot,
                                       self.run_id, self.artifact.target,
                                       self.application, self.workspace, clock + seconds)
            data = contract.write_request(request)
            with open(os.path.join(self.evidence_dir, "request.bin"), "wb") as f:
                f.write(data)
            harness_mailbox.publish(request_path, data)
            self.request = request
            self.seconds = seconds
            self.state = "published"
        return self._step("request", body)

    # ---- collection ------------------------------------------------------

    def result_path(self):
        return os.path.join(self.mailbox_share, contract.filename(self.request, "result"))

    def collect(self, timeout=None, screenshot=True):
        """Wait for the closed result, then retain the attributable evidence."""
        if self.request is None:
            raise PublicationError("publish the request before collecting")
        if timeout is None:
            timeout = (self.seconds or REQUEST_SECONDS) + WAIT_GRACE
        if not 0 < timeout <= 3600:
            raise UsageError("collect seconds must be within 1..3600")
        result_path = self.result_path()

        def wait():
            deadline = self.publisher.clock.monotonic() + timeout
            while self.publisher.clock.monotonic() < deadline:
                try:
                    with open(result_path, "rb") as f:
                        data = f.read(contract.REPORT_CAP)
                except FileNotFoundError:
                    self.publisher.clock.sleep(POLL_SECONDS)
                    continue
                except OSError as exc:
                    raise PublicationError("result unreadable: %s" % exc)
                try:
                    return contract.read_report(data, self.request)
                except contract.ContractError as exc:
                    raise PublicationError("invalid result: %s" % exc)
            raise RunTimeout("no closed result within %.1f seconds; unknown; evidence "
                             "retained under %s" % (timeout, self.evidence_dir))
        report = self._step("collect", wait, require_lease=False)
        self.report = report
        self.state = "settled"
        self._capture(screenshot)
        self._copy_logs()
        self._write_evidence()
        return report

    def _capture(self, screenshot):
        if not screenshot:
            return
        out = os.path.join(self.evidence_dir, "screenshot.ppm")
        try:
            info = self.publisher.manager.screenshot(self.lease.token, self.lease.owner, out)
            self.captures["screenshot"] = info["path"]
        except (reservation.ReservationError, qmp.QMPError, OSError, ValueError) as exc:
            self.captures["screenshot_error"] = str(exc)[:300]

    def _copy_logs(self):
        slot = provision.slot(self.publisher.home, self.lease.slot)
        self.logs["qemu"] = slot.log
        self._copy_log(slot.log, "qemu.log")
        if self.lease.boot:
            trace = os.path.join(self.mailbox_share, "log-" + self.lease.boot[:20])
            self.logs["controller"] = trace
            self._copy_log(trace, "controller.log")
        diagnostic = self.workspace_mount + self.artifact.target + ".log"
        self.logs["diagnostic"] = diagnostic
        self._copy_log(diagnostic, "diagnostic.log")

    def _copy_log(self, source, name):
        data = tail_bytes(source)
        if data is None:
            return
        with open(os.path.join(self.evidence_dir, name), "wb") as f:
            f.write(data)

    # ---- evidence --------------------------------------------------------

    def evidence(self):
        record = {
            "schema": SCHEMA,
            "phase": self.state,
            "error": self.error,
            "run": {"id": self.run_id, "short": self.short, "slot": self.lease.slot,
                    "generation": self.lease.generation,
                    "generation_hex": "%032x" % self.lease.generation,
                    "owner": self.lease.owner, "reservation": self.reservation},
            "boot": {"id": self.lease.boot, "clock": self.lease.boot_clock},
            "manager": dict(self.manager_info or {}),
            "source": self.source,
            "artifact": self.artifact.record(),
            "verification": self.verification,
            "emulator": self.emulator,
            "paths": {"workspace": self.workspace, "application": self.application,
                      "queue": self.queue, "mailbox": self.mailbox_guest,
                      "evidence_dir": self.evidence_dir},
            "fixtures": list(self.fixtures),
            "steps": list(self.steps),
            "captures": dict(self.captures),
            "logs": dict(self.logs),
            "timings": {"started_mono": self.started_mono,
                        "started_wall": self.started_wall,
                        "finished_mono": self.publisher.clock.monotonic(),
                        "finished_wall": self.publisher.clock.wall(),
                        "seconds": self.seconds},
        }
        if self.request is not None:
            record["request"] = {
                "run": self.request.run, "slot": self.request.slot,
                "generation_hex": self.request.generation, "boot": self.request.boot,
                "diagnostic": self.request.diagnostic,
                "application": self.request.application,
                "workspace": self.request.workspace, "deadline": self.request.deadline,
                "saved": os.path.join(self.evidence_dir, "request.bin"),
                "sha256": netatalk_meta.sha256(contract.write_request(self.request)),
            }
        if self.report is not None:
            failures = sum(not ok for ok, _ in self.report.checks)
            record["result"] = {
                "outcome": self.report.outcome,
                "total": len(self.report.checks),
                "failures": failures,
                "checks": [[ok, name] for ok, name in self.report.checks],
                "path": self.result_path(),
            }
        return record

    def _write_evidence(self):
        temporary = self.evidence_path + ".tmp"
        with open(temporary, "w", encoding="utf-8") as f:
            json.dump(self.evidence(), f, indent=2, sort_keys=True)
            f.write("\n")
        os.replace(temporary, self.evidence_path)


def build_parser():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--home", help="harness state directory (default $OS9_HARNESS_HOME)")
    ap.add_argument("--share", help="mounted Retro68 export (default $OS9_HARNESS_SHARE)")
    ap.add_argument("--share-host", help="AFP share host (default $OS9_HARNESS_SHARE_HOST)")
    ap.add_argument("--share-dir", help="server-side export (default %s)" % DEFAULT_SHARE_DIR)
    ap.add_argument("--json", action="store_true", help="structured output")
    # The same flags after a subcommand use SUPPRESS defaults so they win over
    # the root defaults without clobbering values given before it.
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

    p = command("verify", help="validate a built artifact before reserving")
    p.add_argument("--build", required=True)
    p.add_argument("--target", required=True)

    p = command("install", help="publish one artifact to an exact share path")
    p.add_argument("--build", required=True)
    p.add_argument("--target", required=True)
    p.add_argument("--dest", required=True, help="server-side destination file")

    p = command("run", help="publish and collect one diagnostic run")
    p.add_argument("--build", required=True)
    p.add_argument("--target", required=True)
    p.add_argument("--token", required=True)
    p.add_argument("--owner", required=True)
    p.add_argument("--seconds", type=int, default=REQUEST_SECONDS)
    p.add_argument("--wait", type=float, default=None,
                   help="bounded wait for the result (default seconds+15)")
    p.add_argument("--fixture", action="append", default=[],
                   help="NAME=LOCALFILE (repeatable)")
    p.add_argument("--no-screenshot", action="store_true")
    return ap


def emit(args, result):
    if args.json:
        json.dump(result, sys.stdout, indent=2, sort_keys=True)
        sys.stdout.write("\n")
        return
    if args.command == "verify":
        print("target %s" % result["target"])
        print("data_sha256 %s" % result["data_sha256"])
        print("resource_sha256 %s" % result["resource_sha256"])
        if result["source"]["revision"]:
            print("source %s%s" % (result["source"]["revision"],
                                   " (dirty)" if result["source"]["dirty"] else ""))
    elif args.command == "install":
        print("published %s" % result["dest"])
        print("data_sha256 %s" % result["data_sha256"])
    else:
        print("run %s" % result["run"]["id"])
        print("workspace %s" % result["paths"]["workspace"])
        print("evidence %s" % result["paths"]["evidence_dir"])
        if result.get("result"):
            print("result %s checks=%d failures=%d"
                  % (result["result"]["outcome"], result["result"]["total"],
                     result["result"]["failures"]))
        print("phase %s" % result["phase"])


def parse_fixtures(raw):
    fixtures = []
    for spec in raw:
        if "=" not in spec:
            raise UsageError("--fixture must be NAME=LOCALFILE")
        name, path = spec.split("=", 1)
        try:
            with open(path, "rb") as f:
                fixtures.append((name, f.read()))
        except OSError as exc:
            raise UsageError("cannot read fixture %s: %s" % (path, exc))
    return fixtures


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        if args.command == "verify":
            artifact = locate_artifact(args.build, args.target)
            result = artifact.record()
            result["source"] = source_state(os.path.dirname(artifact.build_dir))
            emit(args, result)
            return 0
        if args.command == "install":
            artifact = locate_artifact(args.build, args.target, registered=False)
            host = args.share_host or os.environ.get("OS9_HARNESS_SHARE_HOST")
            share_dir = (args.share_dir or os.environ.get("OS9_HARNESS_SHARE_DIR")
                         or DEFAULT_SHARE_DIR)
            remote = Remote(host, share_dir)
            try:
                state = publish_artifact(remote, artifact, args.dest)
            finally:
                remote.cleanup()
            emit(args, {"dest": args.dest, "data_sha256": artifact.data_sha256,
                        "resource_sha256": artifact.resource_sha256,
                        "problems": netatalk_meta.problems(state)})
            return 0
        artifact = locate_artifact(args.build, args.target)
        fixtures = parse_fixtures(args.fixture)
        publisher = Publisher(home=args.home, share=args.share,
                              share_host=args.share_host, share_dir=args.share_dir)
        run = publisher.begin(args.token, args.owner, artifact)
        try:
            run.publish_application()
            for name, data in fixtures:
                run.publish_fixture(name, data)
            run.publish_request(args.seconds)
            report = run.collect(args.wait, screenshot=not args.no_screenshot)
        finally:
            publisher.cleanup()
        emit(args, run.evidence())
        return 0 if report.outcome == "passed" else 1
    except (UsageError, reservation.UsageError, reservation.LeaseError,
            reservation.NoCapacity, reservation.ConfigError, ConfigError) as exc:
        print("publish: %s" % exc, file=sys.stderr)
        return 2
    except (PublicationError, reservation.ReservationError, provision.ProvisionError,
            contract.ContractError, qmp.QMPError, OSError, ValueError) as exc:
        print("publish: %s" % exc, file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
