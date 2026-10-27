"""Host tests for lease-aware publication and attributable evidence; no share host.

A fake remote stands in for ssh/scp; the reservation manager runs against its
fake effects, so the suites need no QEMU, VM, network or mounted export.
"""
import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import contract  # noqa: E402
import netatalk_meta  # noqa: E402
import provision  # noqa: E402
import publish  # noqa: E402
import reservation  # noqa: E402
from test_reservation import FakeClock, FakeEffects  # noqa: E402


class BuildTree:
    """A local build tree with data and Rez resource forks for one target."""

    def __init__(self, name, data=b"data-fork-bytes", resource=b"resource-fork-bytes"):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = self.tmp.name
        self.name = name
        os.makedirs(os.path.join(self.path, ".rsrc"))
        with open(os.path.join(self.path, name + ".APPL"), "wb") as f:
            f.write(data)
        with open(os.path.join(self.path, ".rsrc", name + ".APPL"), "wb") as f:
            f.write(resource)
        self.data = data
        self.resource = resource

    def close(self):
        self.tmp.cleanup()


class ReadyEffects(FakeEffects):
    """Fake effects that also publish a fresh controller heartbeat on the mount."""

    def __init__(self, home, share, clock):
        super().__init__(home, share, clock)
        self.ready_clock = {}

    def wait_ready(self, slot, previous_boot=None, timeout=reservation.READY_TIMEOUT):
        boot, clock = super().wait_ready(slot, previous_boot, timeout)
        name = slot if isinstance(slot, str) else slot.name
        self.ready_clock[name] = clock
        self.write_ready(name, boot)
        return boot, clock

    def write_ready(self, name, boot, clock=None):
        if clock is not None:
            self.ready_clock[name] = clock
        data = b"READY/1\r%s\r%s\r%d\rEND\r" % (name.encode("ascii"), boot.encode("ascii"),
                                                 self.ready_clock[name])
        path = os.path.join(self.mailbox(name), "ready")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)


class FakeRemote:
    """The share-host boundary, recording every operation for assertions."""

    def __init__(self, host=None, share_dir=None, **kw):
        self.host = host
        self.share_dir = share_dir or publish.DEFAULT_SHARE_DIR
        self.calls = []
        self.fail = {}
        self.corrupt = set()
        self.staged = {}
        self.files = {}
        self.metadata = {}
        self.sentinels = {}
        self.stage_dir = "/tmp/sherclawk-harness-publish.%d" % id(self)
        self.closed = False

    # -- recorded operations ------------------------------------------------

    def _note(self, op, *args):
        self.calls.append((op,) + args)
        if op in self.fail:
            raise publish.RemoteError("%s: %s" % (op, self.fail[op]))

    def staging(self):
        self._note("staging")
        return self.stage_dir

    def stage_bytes(self, leaf, data):
        self._note("stage_bytes", leaf, len(data))
        path = "%s/%s" % (self.stage_dir, leaf)
        self.staged[path] = data
        return path

    def upload(self, local, leaf):
        self._note("upload", leaf)
        with open(local, "rb") as f:
            data = f.read()
        path = "%s/%s" % (self.stage_dir, leaf)
        self.staged[path] = data
        return path

    def read_sentinel(self, export):
        self._note("read_sentinel", export)
        return self.sentinels.get(export)

    def make_dir(self, *paths):
        for path in paths:
            self._note("make_dir", path)

    def install(self, staged, dest):
        self._note("install", staged, dest)
        data = self.staged.get(staged)
        if data is None:
            raise publish.RemoteError("unknown staged file %s" % staged)
        self.files[dest] = data

    def set_metadata(self, dest):
        self._note("set_metadata", dest)
        if dest not in self.files:
            raise publish.RemoteError("no installed file at %s" % dest)
        name = dest.rsplit("/", 1)[1]
        finf = netatalk_meta.make_finderinfo(
            with_dates=True, custom=netatalk_meta.uses_custom_icon(name))
        self.metadata[dest] = netatalk_meta.meta_record(4242, 8484, finf)

    def inspect(self, dest):
        self._note("inspect", dest)
        data = self.files.get(dest)
        if data is None:
            raise publish.RemoteError("no file at %s" % dest)
        if dest in self.corrupt:
            data = b"tampered"
        sidecar = self.files.get(os.path.join(os.path.dirname(dest),
                                              "._" + os.path.basename(dest)))
        return netatalk_meta.state_of(os.path.basename(dest), data, sidecar,
                                      self.metadata.get(dest))

    def cleanup(self):
        self.closed = True

    # -- helpers ------------------------------------------------------------

    def ops(self):
        return [call[0] for call in self.calls]


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = provision.Home(os.path.join(self.tmp.name, "state"))
        self.share = os.path.join(self.tmp.name, "share")
        os.makedirs(os.path.join(self.share, "Harness"))
        self.clock = FakeClock()
        self.effects = ReadyEffects(self.home, self.share, self.clock)
        self.manager = reservation.Manager(home=self.home, clock=self.clock,
                                           effects=self.effects)
        self.remote = FakeRemote()
        self.heartbeat_error = None
        self.publisher = publish.Publisher(home=self.home, share=self.share,
                                           share_host="share-host", remote=self.remote,
                                           manager=self.manager, clock=self.clock,
                                           wait_ready=self.read_heartbeat)
        self.build = BuildTree("SherclawkWriteCheck")
        self.addCleanup(self.build.close)
        self.artifact = publish.locate_artifact(self.build.path, "SherclawkWriteCheck")

    def read_heartbeat(self, mailbox, slot):
        """The live-heartbeat check, reading the static fake readiness file."""
        if self.heartbeat_error:
            raise contract.ContractError(self.heartbeat_error)
        with open(os.path.join(mailbox, "ready"), "rb") as f:
            return contract.read_ready_clock(f.read(129), slot)

    def lease(self, owner="alice"):
        return self.manager.reserve(owner)

    def begin(self, owner="alice", lease=None):
        lease = lease or self.lease(owner)
        return lease, self.publisher.begin(lease.token, owner, self.artifact)

    def result_data(self, run, outcome="passed", checks=((True, "one check"),)):
        report = contract.Report(run.request, outcome, list(checks))
        return contract.write_report(report)

    def publish_result(self, run, **kw):
        data = self.result_data(run, **kw)
        path = os.path.join(run.mailbox_share, contract.filename(run.request, "result"))
        with open(path, "wb") as f:
            f.write(data)
        return path

    def evidence(self, run):
        with open(run.evidence_path) as f:
            return json.load(f)


class IsolatedBase(Base):
    """Two slots on disjoint volumes, each marked on the mount and on the server."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = provision.Home(os.path.join(self.tmp.name, "state"))
        self.mounts = {}
        self.exports = {}
        self.remote = FakeRemote()
        for name in provision.SLOTS:
            self.mounts[name] = os.path.join(self.tmp.name, "mnt-" + name)
            self.exports[name] = "/srv/harness/" + name
            os.makedirs(os.path.join(self.mounts[name], "Harness", name))
            self.mark_mount(name, name)
            self.remote.sentinels[self.exports[name]] = name
            provision.set_storage(self.home, name, self.mounts[name], self.exports[name])
        self.clock = FakeClock()
        self.effects = ReadyEffects(self.home, None, self.clock)
        self.manager = reservation.Manager(home=self.home, clock=self.clock,
                                           effects=self.effects)
        self.heartbeat_error = None
        self.publisher = publish.Publisher(home=self.home, share_host="share-host",
                                           remote=self.remote, manager=self.manager,
                                           clock=self.clock, wait_ready=self.read_heartbeat)
        self.build = BuildTree("SherclawkWriteCheck")
        self.addCleanup(self.build.close)
        self.artifact = publish.locate_artifact(self.build.path, "SherclawkWriteCheck")

    def mark_mount(self, mount_slot, content):
        with open(os.path.join(self.mounts[mount_slot], provision.SENTINEL), "w") as f:
            f.write(content + "\n")

    def written(self):
        return [call for call in self.remote.calls
                if call[0] in ("staging", "stage_bytes", "make_dir", "install")]


class IsolatedPublicationTest(IsolatedBase):
    def test_each_lease_publishes_only_into_its_own_volume(self):
        lease_a, run_a = self.begin("alice")
        lease_b, run_b = self.begin("bob")
        self.assertEqual((lease_a.slot, lease_b.slot), ("slot1", "slot2"))
        run_a.publish_application()
        run_b.publish_application()
        for run, slot in ((run_a, "slot1"), (run_b, "slot2")):
            self.assertTrue(run.mailbox_share.startswith(self.mounts[slot] + os.sep))
            self.assertTrue(run.workspace_mount.startswith(self.mounts[slot] + os.sep))
        dests = {slot: [c[2] for c in self.remote.calls if c[0] == "install"
                        and c[2].startswith(self.exports[slot] + "/")]
                 for slot in provision.SLOTS}
        installs = [c[2] for c in self.remote.calls if c[0] == "install"]
        self.assertEqual(len(installs), 4)
        for slot in provision.SLOTS:
            self.assertEqual(len(dests[slot]), 2, slot)
        self.assertTrue(all(path.startswith("/srv/harness/") for path in installs))
        self.assertFalse(any(path.startswith("/srv/retro68") for path in installs))

    def test_workspace_directories_are_made_under_the_slots_export(self):
        lease, run = self.begin()
        run.publish_application()
        dirs = [c[1] for c in self.remote.calls if c[0] == "make_dir"]
        self.assertEqual(dirs[0], "%s/Harness/%s" % (self.exports[lease.slot], run.reservation))
        self.assertTrue(all(d.startswith(self.exports[lease.slot] + "/Harness/") for d in dirs))

    def test_evidence_names_the_volume_the_run_used(self):
        lease, run = self.begin()
        run.publish_application()
        storage = self.evidence(run)["storage"]
        self.assertEqual(storage["slot"], lease.slot)
        self.assertEqual(storage["mount"], self.mounts[lease.slot])
        self.assertEqual(storage["export"], self.exports[lease.slot])
        self.assertTrue(storage["isolated"])

    def test_the_request_lands_in_the_slots_own_mailbox_only(self):
        lease, run = self.begin()
        run.publish_application()
        run.publish_request(seconds=60)
        self.assertTrue(os.path.exists(os.path.join(self.mounts[lease.slot], "Harness",
                                                    lease.slot, "request")))
        other = "slot2" if lease.slot == "slot1" else "slot1"
        self.assertFalse(os.path.exists(os.path.join(self.mounts[other], "Harness", other,
                                                     "request")))


class StorageMismatchTest(IsolatedBase):
    def test_a_mount_of_another_slots_volume_is_rejected_before_any_write(self):
        lease = self.lease()
        self.mark_mount(lease.slot, "slot2")
        with self.assertRaisesRegex(publish.ConfigError, "slot 'slot2'"):
            self.publisher.begin(lease.token, lease.owner, self.artifact)
        self.assertEqual(self.written(), [])

    def test_an_unmarked_mount_such_as_the_developer_share_is_rejected(self):
        os.remove(os.path.join(self.mounts["slot1"], provision.SENTINEL))
        with self.assertRaisesRegex(reservation.ConfigError, "unmarked"):
            self.lease()

    def test_an_export_that_is_not_the_slots_own_is_rejected_before_any_write(self):
        lease = self.lease()
        self.remote.sentinels[self.exports[lease.slot]] = "slot2"
        with self.assertRaisesRegex(publish.ConfigError, "not this slot's volume"):
            self.publisher.begin(lease.token, lease.owner, self.artifact)
        self.assertEqual(self.written(), [])

    def test_an_unmarked_or_unreachable_export_is_rejected(self):
        lease = self.lease()
        del self.remote.sentinels[self.exports[lease.slot]]
        with self.assertRaisesRegex(publish.ConfigError, "unmarked"):
            self.publisher.begin(lease.token, lease.owner, self.artifact)
        self.assertEqual(self.written(), [])

    def test_remapping_after_begin_is_caught_before_the_application_is_published(self):
        lease, run = self.begin()
        self.remote.sentinels[self.exports[lease.slot]] = "slot2"
        with self.assertRaisesRegex(publish.ConfigError, "not this slot's volume"):
            run.publish_application()
        self.assertEqual(self.written(), [])
        self.assertEqual(self.evidence(run)["phase"], "failed")

    def test_remapping_after_the_application_is_caught_before_the_request(self):
        lease, run = self.begin()
        run.publish_application()
        self.mark_mount(lease.slot, "slot2")
        with self.assertRaisesRegex(publish.ConfigError, "slot 'slot2'"):
            run.publish_request(seconds=60)
        self.assertFalse(os.path.exists(os.path.join(run.mailbox_share, "request")))

    def test_every_slot_must_be_mapped_before_publishing(self):
        os.remove(self.home.storage_path)
        publisher = publish.Publisher(home=self.home, share_host="share-host",
                                      remote=self.remote, manager=self.manager,
                                      clock=self.clock)
        with self.assertRaisesRegex(publish.ConfigError, "no slot storage is mapped"):
            publisher.require_ready()

    def test_a_legacy_single_root_skips_the_sentinel_check(self):
        share = os.path.join(self.tmp.name, "legacy")
        os.makedirs(share)
        storage = provision.Storage.single(share, "/srv/retro68")
        publisher = publish.Publisher(home=self.home, share_host="share-host",
                                      remote=self.remote, manager=self.manager,
                                      clock=self.clock, storage=storage)
        publisher.verify_storage("slot1")
        self.assertNotIn("read_sentinel", self.remote.ops())


class ArtifactTest(unittest.TestCase):
    def setUp(self):
        self.build = BuildTree("SherclawkInspectCheck")
        self.addCleanup(self.build.close)

    def test_unregistered_targets_are_rejected(self):
        with self.assertRaises(publish.ArtifactError) as caught:
            publish.locate_artifact(self.build.path, "SherclawkWhatever")
        self.assertIn("registered", str(caught.exception))

    def test_missing_and_empty_forks_are_rejected(self):
        with self.assertRaises(publish.ArtifactError):
            publish.locate_artifact(self.build.path, "SherclawkWriteCheck")
        os.makedirs(os.path.join(self.build.path, ".rsrc"), exist_ok=True)
        with open(os.path.join(self.build.path, "SherclawkWriteCheck.APPL"), "wb") as f:
            f.write(b"")
        with open(os.path.join(self.build.path, ".rsrc", "SherclawkWriteCheck.APPL"), "wb") as f:
            f.write(b"x")
        with self.assertRaises(publish.ArtifactError):
            publish.locate_artifact(self.build.path, "SherclawkWriteCheck")

    def test_artifact_hashes_both_forks(self):
        artifact = publish.locate_artifact(self.build.path, self.build.name)
        self.assertEqual(artifact.data_sha256, netatalk_meta.sha256(self.build.data))
        self.assertEqual(artifact.resource_sha256, netatalk_meta.sha256(self.build.resource))
        self.assertEqual(artifact.data_bytes, len(self.build.data))
        self.assertEqual(artifact.target, self.build.name)


class SourceStateTest(unittest.TestCase):
    def test_missing_checkout_reports_an_error_and_no_revision(self):
        with tempfile.TemporaryDirectory() as tmp:
            state = publish.source_state(tmp)
        self.assertIsNone(state["revision"])
        self.assertTrue(state["error"])

    def test_revision_and_dirty_state_come_from_git(self):
        if not shutil.which("git"):
            self.skipTest("git is not installed")
        with tempfile.TemporaryDirectory() as tmp:
            env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
                       GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
            subprocess.run(["git", "init", "-q"], cwd=tmp, check=True, env=env)
            with open(os.path.join(tmp, "file"), "w") as f:
                f.write("one\n")
            subprocess.run(["git", "add", "file"], cwd=tmp, check=True, env=env)
            subprocess.run(["git", "commit", "-q", "-m", "one"], cwd=tmp, check=True, env=env)
            state = publish.source_state(tmp)
            self.assertEqual(len(state["revision"]), 40)
            self.assertFalse(state["dirty"])
            with open(os.path.join(tmp, "file"), "a") as f:
                f.write("two\n")
            self.assertTrue(publish.source_state(tmp)["dirty"])

    def test_build_tree_inside_a_checkout_is_attributed_to_it(self):
        if not shutil.which("git"):
            self.skipTest("git is not installed")
        with tempfile.TemporaryDirectory() as tmp:
            repo = os.path.realpath(tmp)
            env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@t",
                       GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@t")
            subprocess.run(["git", "init", "-q"], cwd=repo, check=True, env=env)
            with open(os.path.join(repo, "file"), "w") as f:
                f.write("one\n")
            subprocess.run(["git", "add", "file"], cwd=repo, check=True, env=env)
            subprocess.run(["git", "commit", "-q", "-m", "one"], cwd=repo, check=True, env=env)
            build = os.path.join(repo, "elsewhere", "build")
            os.makedirs(build)
            state = publish.source_state(build, built_at=time.time() + 60)
            self.assertEqual(state["repo"], repo)
            self.assertEqual(len(state["revision"]), 40)
            self.assertFalse(state["artifact_predates_head"])
            self.assertFalse(state["artifact_predates_changes"])
            # Built before HEAD was committed, and an untracked source is newer.
            state = publish.source_state(build, built_at=1.0)
            self.assertTrue(state["artifact_predates_head"])
            with open(os.path.join(repo, "new.c"), "w") as f:
                f.write("int x;\n")
            state = publish.source_state(build, built_at=time.time() - 60)
            self.assertTrue(state["dirty"])
            self.assertTrue(state["artifact_predates_changes"])


class LeaseGateTest(Base):
    def test_begin_rejects_a_stale_token_before_creating_evidence(self):
        lease = self.lease()
        evidence_root = self.publisher.evidence_dir
        with self.assertRaises(reservation.LeaseError):
            self.publisher.begin("f" * 32, "alice", self.artifact)
        self.assertFalse(os.path.exists(os.path.join(evidence_root, lease.slot)))

    def test_begin_rejects_an_expired_lease(self):
        lease = self.lease()
        self.clock.advance(601)
        with self.assertRaises(reservation.LeaseError):
            self.publisher.begin(lease.token, "alice", self.artifact)

    def test_begin_rejects_a_missing_share_or_host(self):
        with self.assertRaises(publish.ConfigError):
            publish.Publisher(home=self.home, share=None, remote=self.remote,
                              manager=self.manager).begin("a" * 32, "alice", self.artifact)
        with self.assertRaises(publish.ConfigError):
            publish.Publisher(home=self.home, share=self.share, share_host=None,
                              manager=self.manager).begin("a" * 32, "alice", self.artifact)

    def test_run_paths_are_private_and_never_reused(self):
        lease_a, run_a = self.begin()
        first = (run_a.workspace, run_a.application, run_a.queue, run_a.evidence_dir)
        self.manager.release(lease_a.token, "alice")
        lease_b, run_b = self.begin("bob")
        second = (run_b.workspace, run_b.application, run_b.queue, run_b.evidence_dir)
        self.assertEqual(len(set(first + second)), 8)
        self.assertNotEqual(run_a.run_id, run_b.run_id)
        self.assertNotEqual(run_a.reservation, run_b.reservation)
        self.assertTrue(run_a.workspace.startswith("Retro68:Harness:"))
        self.assertTrue(run_a.workspace.endswith(":"))
        self.assertEqual(run_a.application, run_a.workspace + "SherclawkWriteCheck")
        self.assertTrue(run_a.queue.startswith(run_a.workspace))
        self.assertTrue(run_a.queue.endswith(":"))


class PublicationTest(Base):
    def test_application_publication_installs_both_forks_then_verifies(self):
        _lease, run = self.begin()
        run.publish_application()
        ops = self.remote.ops()
        self.assertLess(ops.index("make_dir"), ops.index("install"))
        self.assertLess(ops.index("install"), ops.index("set_metadata"))
        self.assertLess(ops.index("set_metadata"), ops.index("inspect"))
        dirs = [call[1] for call in self.remote.calls if call[0] == "make_dir"]
        self.assertEqual(dirs, ["/srv/retro68/Harness/%s" % run.reservation,
                                "/srv/retro68/Harness/%s/%s" % (run.reservation, run.short),
                                "/srv/retro68/Harness/%s/%s/buildjobs" % (run.reservation, run.short)])
        dests = [call[2] for call in self.remote.calls if call[0] == "install"]
        app = "/srv/retro68/Harness/%s/%s/SherclawkWriteCheck" % (run.reservation, run.short)
        self.assertEqual(dests, [app, os.path.join(os.path.dirname(app), "._SherclawkWriteCheck")])
        self.assertEqual(self.remote.files[app], self.build.data)
        sidecar = self.remote.files[os.path.join(os.path.dirname(app), "._SherclawkWriteCheck")]
        self.assertEqual(netatalk_meta.parse_sidecar(sidecar)["resource"],
                         self.build.resource)
        state = self.evidence(run)["verification"]
        self.assertEqual(netatalk_meta.problems(state), [])

    def test_each_staging_and_publication_boundary_fails_closed(self):
        for boundary in ("make_dir", "stage_bytes", "install", "set_metadata", "inspect"):
            with self.subTest(boundary=boundary):
                lease, run = self.begin()
                self.remote.fail[boundary] = "boom"
                with self.assertRaises(publish.PublicationError):
                    run.publish_application()
                self.remote.fail.clear()
                request_path = os.path.join(run.mailbox_share, "request")
                self.assertFalse(os.path.exists(request_path),
                                 "%s yielded a ready request" % boundary)
                record = self.evidence(run)
                self.assertEqual(record["phase"], "failed")
                self.assertIn(boundary, json.dumps(record["steps"]))
                self.manager.release(lease.token, lease.owner)

    def test_a_tampered_remote_artifact_never_reaches_the_request(self):
        _lease, run = self.begin()
        app = "/srv/retro68/Harness/%s/%s/SherclawkWriteCheck" % (run.reservation, run.short)
        self.remote.corrupt.add(app)
        with self.assertRaises(publish.PublicationError) as caught:
            run.publish_application()
        self.assertIn("data fork", str(caught.exception))
        self.assertFalse(os.path.exists(os.path.join(run.mailbox_share, "request")))

    def test_the_request_cannot_precede_a_verified_application(self):
        _lease, run = self.begin()
        with self.assertRaises(publish.PublicationError):
            run.publish_request()
        self.assertFalse(os.path.exists(os.path.join(run.mailbox_share, "request")))
        self.remote.fail["inspect"] = "boom"
        with self.assertRaises(publish.PublicationError):
            run.publish_application()
        self.remote.fail.clear()
        with self.assertRaises(publish.PublicationError):
            run.publish_request(seconds=60)
        self.assertFalse(os.path.exists(os.path.join(run.mailbox_share, "request")))

    def test_fixtures_are_validated_and_recorded(self):
        _lease, run = self.begin()
        run.publish_application()
        with self.assertRaises(publish.UsageError):
            run.publish_fixture("bad:name", b"x")
        with self.assertRaises(publish.UsageError):
            run.publish_fixture("", b"x")
        with self.assertRaises(publish.UsageError):
            run.publish_fixture("x" * 32, b"x")
        with self.assertRaises(publish.UsageError):
            run.publish_fixture(".hidden", b"x")
        with self.assertRaises(publish.UsageError):
            run.publish_fixture("scenario", b"x" * (publish.FIXTURE_MAX + 1))
        for bad in ("a/b", "a/../../x", "SherclawkWriteCheck", "buildjobs",
                    "SherclawkWriteCheck.log"):
            with self.assertRaises(publish.UsageError, msg=bad):
                run.publish_fixture(bad, b"x")
        run.publish_fixture("my file", b"spaced")  # a legal HFS name, not a staging leaf
        run.publish_fixture("scenario", b"pass\r")
        with self.assertRaises(publish.UsageError):
            run.publish_fixture("scenario", b"again")
        record = self.evidence(run)
        self.assertEqual(record["fixtures"][1:], [{
            "name": "scenario",
            "bytes": 5,
            "sha256": netatalk_meta.sha256(b"pass\r"),
            "guest": run.workspace + "scenario",
            "remote": "/srv/retro68/Harness/%s/%s/scenario" % (run.reservation, run.short),
        }])
        self.assertEqual(self.remote.files[record["fixtures"][1]["remote"]], b"pass\r")

    def test_fixture_installation_failure_publishes_no_request(self):
        _lease, run = self.begin()
        run.publish_application()
        self.remote.fail["install"] = "disk full"
        with self.assertRaises(publish.PublicationError):
            run.publish_fixture("scenario", b"pass\r")
        self.assertFalse(os.path.exists(os.path.join(run.mailbox_share, "request")))


class RequestTest(Base):
    def test_request_publication_needs_an_empty_queue(self):
        _lease, run = self.begin()
        run.publish_application()
        with open(os.path.join(run.mailbox_share, "request"), "wb") as f:
            f.write(b"leftover")
        with self.assertRaises(publish.PublicationError) as caught:
            run.publish_request()
        self.assertIn("unconsumed request", str(caught.exception))
        with open(os.path.join(run.mailbox_share, "request"), "rb") as f:
            self.assertEqual(f.read(), b"leftover")

    def test_request_rejects_a_changed_boot(self):
        lease, run = self.begin()
        run.publish_application()
        self.effects.write_ready(lease.slot, "f" * 32)
        with self.assertRaises(publish.PublicationError) as caught:
            run.publish_request()
        self.assertIn("boot", str(caught.exception))
        self.assertFalse(os.path.exists(os.path.join(run.mailbox_share, "request")))

    def test_request_needs_a_live_heartbeat(self):
        _lease, run = self.begin()
        run.publish_application()
        self.heartbeat_error = "no advancing fresh controller heartbeat"
        with self.assertRaises(publish.PublicationError) as caught:
            run.publish_request()
        self.assertIn("heartbeat", str(caught.exception))
        self.assertFalse(os.path.exists(os.path.join(run.mailbox_share, "request")))

    def test_request_rejects_an_expired_lease(self):
        lease, run = self.begin()
        run.publish_application()
        self.clock.advance(601)
        with self.assertRaises(reservation.LeaseError):
            run.publish_request()
        self.assertFalse(os.path.exists(os.path.join(run.mailbox_share, "request")))

    def test_request_binds_the_exact_run_to_the_lease(self):
        lease, run = self.begin()
        run.publish_application()
        run.publish_fixture("scenario", b"pass\r")
        run.publish_request(seconds=90)
        with open(os.path.join(run.mailbox_share, "request"), "rb") as f:
            raw = f.read()
        request = contract.read_request(raw)
        self.assertEqual(request.slot, lease.slot)
        self.assertEqual(request.generation, "%032x" % lease.generation)
        self.assertEqual(request.boot, lease.boot)
        self.assertEqual(request.run, run.run_id)
        self.assertEqual(request.diagnostic, "SherclawkWriteCheck")
        self.assertEqual(request.application, run.application)
        self.assertEqual(request.workspace, run.workspace)
        self.assertEqual(request.deadline,
                         self.effects.ready_clock[lease.slot] + 90)
        with open(os.path.join(run.evidence_dir, "request.bin"), "rb") as f:
            self.assertEqual(f.read(), raw)
        record = self.evidence(run)
        self.assertEqual(record["request"]["run"], run.run_id)
        self.assertEqual(record["request"]["deadline"], request.deadline)
        self.assertEqual(record["phase"], "published")
        self.assertEqual(record["run"]["generation_hex"], "%032x" % lease.generation)

    def test_two_reservations_publish_concurrently_without_collisions(self):
        lease_a = self.lease("alice")
        lease_b = self.lease("bob")
        remotes = [FakeRemote(), FakeRemote()]
        publishers = [
            publish.Publisher(home=self.home, share=self.share, share_host="share-host",
                              remote=remotes[index], manager=self.manager, clock=self.clock,
                              wait_ready=self.read_heartbeat)
            for index in range(2)]
        runs = [publishers[0].begin(lease_a.token, "alice", self.artifact),
                publishers[1].begin(lease_b.token, "bob", self.artifact)]
        failures = []

        def work(run):
            try:
                run.publish_application()
                run.publish_fixture("scenario", b"pass\r")
                run.publish_request()
            except BaseException as exc:  # pragma: no cover - assertion below
                failures.append(exc)

        threads = [threading.Thread(target=work, args=(run,)) for run in runs]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(failures, [])
        requests = {}
        for run in runs:
            path = os.path.join(run.mailbox_share, "request")
            with open(path, "rb") as f:
                requests[run.lease.slot] = contract.read_request(f.read())
            self.assertIn(run.short, run.evidence_dir)
        self.assertEqual(requests["slot1"].workspace, runs[0].workspace)
        self.assertEqual(requests["slot2"].workspace, runs[1].workspace)
        self.assertNotEqual(runs[0].workspace, runs[1].workspace)
        dests = [call[2] for remote in remotes for call in remote.calls if call[0] == "install"]
        self.assertEqual(len(dests), 6)
        self.assertEqual(len(dests), len(set(dests)))
        self.assertEqual(len([d for d in dests if d.endswith("/SherclawkWriteCheck")]), 2)
        self.assertNotEqual(remotes[0].stage_dir, remotes[1].stage_dir)
        for run in runs:
            with open(run.evidence_path) as f:
                self.assertEqual(json.load(f)["phase"], "published")


class CollectTest(Base):
    def publish(self, owner="alice"):
        lease, run = self.begin(owner)
        run.publish_application()
        run.publish_fixture("scenario", b"pass\r")
        run.publish_request(seconds=120)
        return lease, run

    def consume_request(self, run):
        """The controller's claim: request is renamed to claim-<run prefix>."""
        os.rename(os.path.join(run.mailbox_share, "request"),
                  os.path.join(run.mailbox_share, contract.filename(run.request, "claim")))

    def test_collect_reads_the_result_and_records_the_capture(self):
        lease, run = self.publish()
        slot = provision.slot(self.home, lease.slot)
        os.makedirs(os.path.dirname(slot.log), exist_ok=True)
        with open(slot.log, "wb") as f:
            f.write(b"qemu log line\n")
        with open(os.path.join(run.mailbox_share, "log-" + lease.boot[:20]), "wb") as f:
            f.write(b"controller trace\n")
        os.makedirs(run.workspace_mount, exist_ok=True)
        with open(run.workspace_mount + "SherclawkWriteCheck.log", "wb") as f:
            f.write(b"diagnostic log\n")
        self.publish_result(run, outcome="failed", checks=((True, "alpha"), (False, "beta")))
        report = run.collect(timeout=5)
        self.assertEqual(report.outcome, "failed")
        self.assertEqual(report.checks, [(True, "alpha"), (False, "beta")])
        record = self.evidence(run)
        self.assertEqual(record["phase"], "settled")
        self.assertEqual(record["result"]["outcome"], "failed")
        self.assertEqual(record["result"]["total"], 2)
        self.assertEqual(record["result"]["failures"], 1)
        self.assertEqual(record["captures"]["screenshot"],
                         os.path.join(run.evidence_dir, "screenshot.ppm"))
        self.assertTrue(os.path.exists(record["captures"]["screenshot"]))
        self.assertTrue(os.path.exists(os.path.join(run.evidence_dir, "qemu.log")))
        self.assertTrue(os.path.exists(os.path.join(run.evidence_dir, "controller.log")))
        self.assertTrue(os.path.exists(os.path.join(run.evidence_dir, "diagnostic.log")))
        self.assertEqual(record["logs"]["qemu"], slot.log)
        self.assertEqual(record["paths"]["workspace"], run.workspace)
        self.assertIn("finished_mono", record["timings"])

    def test_collect_polls_the_caller_while_waiting(self):
        _lease, run = self.publish()
        calls = []

        def poll():
            calls.append(1)
            if len(calls) == 2:
                self.publish_result(run)

        report = run.collect(timeout=5, on_poll=poll)
        self.assertEqual(report.outcome, "passed")
        self.assertEqual(len(calls), 2)

    def test_collect_timeout_preserves_evidence_and_never_passes(self):
        _lease, run = self.publish()
        with self.assertRaises(publish.RunTimeout) as caught:
            run.collect(timeout=0.2)
        self.assertIn("unknown", str(caught.exception))
        record = self.evidence(run)
        self.assertEqual(record["phase"], "unknown")
        self.assertEqual(record["error"], str(caught.exception))
        self.assertNotIn("result", record)

    def test_failed_collection_still_retains_screenshot_and_logs(self):
        lease, run = self.publish()
        slot = provision.slot(self.home, lease.slot)
        os.makedirs(os.path.dirname(slot.log), exist_ok=True)
        with open(slot.log, "wb") as f:
            f.write(b"qemu log line\n")
        with open(os.path.join(run.mailbox_share, "log-" + lease.boot[:20]), "wb") as f:
            f.write(b"controller trace\n")
        with self.assertRaises(publish.RunTimeout):
            run.collect(timeout=0.2)
        record = self.evidence(run)
        self.assertTrue(os.path.exists(record["captures"]["screenshot"]))
        self.assertTrue(os.path.exists(os.path.join(run.evidence_dir, "qemu.log")))
        self.assertTrue(os.path.exists(os.path.join(run.evidence_dir, "controller.log")))
        # An invalid closed result is retained the same way.
        lease, run = self.begin("bob")
        run.publish_application()
        run.publish_request(seconds=120)
        with open(run.result_path(), "wb") as f:
            f.write(b"not a result")
        with self.assertRaises(publish.PublicationError):
            run.collect(timeout=5)
        self.assertTrue(os.path.exists(self.evidence(run)["captures"]["screenshot"]))

    def test_collect_after_expiry_still_settles_with_available_evidence(self):
        lease, run = self.publish()
        self.publish_result(run)
        self.clock.advance(601)
        report = run.collect(timeout=5)
        self.assertEqual(report.outcome, "passed")
        record = self.evidence(run)
        self.assertEqual(record["phase"], "settled")
        self.assertNotIn("screenshot", record["captures"])
        self.assertIn("lease not held", record["captures"]["shared_evidence_skipped"])
        self.assertNotIn("qemu", record["logs"])
        self.assertFalse(os.path.exists(os.path.join(run.evidence_dir, "qemu.log")))

    def test_repeated_launches_keep_separate_evidence(self):
        lease_a, run_a = self.publish()
        self.publish_result(run_a)
        run_a.collect(timeout=5)
        first = self.evidence(run_a)
        self.consume_request(run_a)
        self.manager.release(lease_a.token, "alice")
        lease_b, run_b = self.begin("bob")
        run_b.publish_application()
        run_b.publish_request(seconds=120)
        self.publish_result(run_b, outcome="failed", checks=((False, "broken"),))
        run_b.collect(timeout=5)
        second = self.evidence(run_b)
        self.assertEqual(first["phase"], "settled")
        self.assertEqual(second["phase"], "settled")
        self.assertNotEqual(first["run"]["id"], second["run"]["id"])
        self.assertNotEqual(run_a.evidence_dir, run_b.evidence_dir)
        with open(run_a.evidence_path) as f:
            self.assertEqual(json.load(f)["run"]["id"], run_a.run_id)
        result_a = os.path.join(run_a.mailbox_share,
                                contract.filename(run_a.request, "result"))
        result_b = os.path.join(run_b.mailbox_share,
                                contract.filename(run_b.request, "result"))
        self.assertTrue(os.path.exists(result_a))
        self.assertTrue(os.path.exists(result_b))
        self.assertNotEqual(result_a, result_b)


class RemoteTest(unittest.TestCase):
    """The real ssh/scp boundary against a recording subprocess.run."""

    def setUp(self):
        self.runs = []

        def run(argv, **kw):
            self.runs.append(list(argv))
            if argv[0] == "ssh" and "mktemp -d" in argv[-1]:
                return subprocess.CompletedProcess(argv, 0, stdout="/tmp/"
                                                   + publish.STAGE_PREFIX + "ab12cd\n")
            if argv[0] == "ssh" and " check " in argv[-1]:
                return subprocess.CompletedProcess(argv, 0, stdout=json.dumps(
                    {"state": {"name": "app"}, "problems": []}))
            return subprocess.CompletedProcess(argv, 0, stdout="", stderr="")

        patch = mock.patch.object(publish.subprocess, "run", run)
        patch.start()
        self.addCleanup(patch.stop)
        self.remote = publish.Remote("share-host")

    def commands(self):
        return [argv[-1] for argv in self.runs if argv[0] == "ssh"]

    def test_staging_is_created_once_per_remote(self):
        self.assertEqual(self.remote.staging(), "/tmp/sherclawk-harness-publish.ab12cd")
        self.assertEqual(self.remote.staging(), "/tmp/sherclawk-harness-publish.ab12cd")
        mktemp = [c for c in self.commands() if "mktemp" in c]
        self.assertEqual(mktemp, ["mktemp -d /tmp/sherclawk-harness-publish.XXXXXX"])
        self.assertEqual(self.runs[0], ["ssh", "-o", "BatchMode=yes", "share-host",
                                        "mktemp -d /tmp/sherclawk-harness-publish.XXXXXX"])

    def test_directory_and_installation_commands_are_exact(self):
        self.remote.make_dir("/srv/retro68/Harness/run one")
        self.remote.install("/tmp/stage data", "/srv/retro68/app")
        self.assertEqual(self.commands(), [
            "sudo -n install -d -o macos9 -g macos9 -m 0755 '/srv/retro68/Harness/run one'",
            "sudo -n install -o macos9 -g macos9 -m 0666 '/tmp/stage data' /srv/retro68/app"])

    def test_every_directory_level_is_named_for_the_owner(self):
        self.remote.make_dir("/srv/a", "/srv/a/b")
        self.assertEqual(self.commands(),
                         ["sudo -n install -d -o macos9 -g macos9 -m 0755 /srv/a /srv/a/b"])

    def test_metadata_and_inspection_run_the_staged_script(self):
        self.remote.stage_bytes("meta", b"# script\n")
        self.remote.set_metadata("/srv/retro68/app")
        self.remote.inspect("/srv/retro68/app")
        xattr = [c for c in self.commands() if "xattr" in c][0]
        check = [c for c in self.commands() if " check " in c][0]
        self.assertIn("/netatalk_meta.py xattr /srv/retro68/app", xattr)
        self.assertIn("/netatalk_meta.py check /srv/retro68/app", check)
        self.assertIn("sudo -n python3", xattr)

    def test_inspect_parses_the_check_json_even_when_it_reports_problems(self):
        self.remote.stage_bytes("seed", b"x")
        self.remote.meta_script()
        payload = json.dumps({"state": {"name": "app"}, "problems": ["no sidecar"]})
        with mock.patch.object(publish.subprocess, "run",
                               return_value=subprocess.CompletedProcess([], 1, stdout=payload)):
            self.assertEqual(self.remote.inspect("/srv/retro68/app"), {"name": "app"})
        with mock.patch.object(publish.subprocess, "run",
                               return_value=subprocess.CompletedProcess([], 1, stdout="boom")):
            with self.assertRaises(publish.RemoteError):
                self.remote.inspect("/srv/retro68/app")

    def test_cleanup_removes_the_staging_directory(self):
        stage = self.remote.staging()
        self.remote.cleanup()
        self.assertIn("rm -rf %s" % stage, self.commands())
        self.remote.cleanup()
        self.assertEqual(len([c for c in self.commands() if "rm -rf" in c]), 1)

    def test_cleanup_forgets_the_uploaded_script_with_its_directory(self):
        first = self.remote.meta_script()
        self.remote.cleanup()
        self.assertEqual(self.remote.meta_script(), first)  # same fake stage name
        self.assertEqual(len([c for c in self.commands() if "mktemp" in c]), 2)


class CliTest(unittest.TestCase):
    def setUp(self):
        self.build = BuildTree("SherclawkInspectCheck")
        self.addCleanup(self.build.close)

    def invoke(self, argv, publisher=None, remote=None):
        out, err = io.StringIO(), io.StringIO()
        patches = []
        if publisher is not None:
            patches.append(mock.patch.object(publish, "Publisher", return_value=publisher))
        if remote is not None:
            patches.append(mock.patch.object(publish, "Remote", return_value=remote))
        with contextlib.ExitStack() as stack:
            for patch in patches:
                stack.enter_context(patch)
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                code = publish.main(argv)
        return code, out.getvalue(), err.getvalue()

    def test_install_publishes_an_application_through_the_remote(self):
        build = BuildTree("Sherclawk")
        self.addCleanup(build.close)
        remote = FakeRemote()
        code, out, _err = self.invoke(
            ["install", "--build", build.path, "--target", "Sherclawk",
             "--share-host", "share-host", "--dest", "/srv/retro68/Sherclawk", "--json"],
            remote=remote)
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out)["data_sha256"],
                         netatalk_meta.sha256(build.data))
        self.assertEqual(remote.files["/srv/retro68/Sherclawk"], build.data)
        self.assertIn("/srv/retro68/._Sherclawk", remote.files)
        self.assertTrue(remote.closed)
        self.assertEqual(remote.ops(), ["stage_bytes", "stage_bytes", "install", "install",
                                        "set_metadata", "inspect"])

    def test_run_and_verify_reject_an_unregistered_target(self):
        build = BuildTree("Sherclawk")
        self.addCleanup(build.close)
        for argv in (
                ["verify", "--build", build.path, "--target", "Sherclawk"],
                self.run_argv(target="Sherclawk")):
            code, _out, err = self.invoke(argv, publisher=self.StubPublisher(self.StubRun()))
            self.assertEqual(code, 3)
            self.assertIn("registered", err)

    def test_verify_prints_the_artifact_hashes(self):
        code, out, _err = self.invoke(
            ["verify", "--build", self.build.path, "--target", "SherclawkInspectCheck",
             "--json"])
        self.assertEqual(code, 0)
        data = json.loads(out)
        self.assertEqual(data["target"], "SherclawkInspectCheck")
        self.assertEqual(data["data_sha256"], netatalk_meta.sha256(self.build.data))

    def test_verify_rejects_an_unregistered_target(self):
        code, _out, err = self.invoke(
            ["verify", "--build", self.build.path, "--target", "Nope"])
        self.assertEqual(code, 3)
        self.assertIn("registered", err)

    class StubRun:
        def __init__(self, report=None, error=None):
            self.report = report
            self.error = error
            self.calls = []
            self.run_id = "a" * 32
            self.workspace = "Retro68:Harness:slot1-g1:aaaa:"
            self.evidence_path = "/tmp/evidence.json"
            self.evidence_dir = "/tmp/evidence"
            self.state = "begun"

        def publish_application(self):
            self.calls.append("application")
            if self.error:
                raise self.error

        def publish_fixture(self, name, data):
            self.calls.append(("fixture", name, data))

        def publish_request(self, seconds):
            self.calls.append(("request", seconds))

        def collect(self, timeout=None, screenshot=True):
            self.calls.append("collect")
            return self.report

        def evidence(self):
            record = {"run": {"id": self.run_id},
                      "paths": {"workspace": self.workspace,
                                "evidence_dir": self.evidence_path},
                      "phase": "settled"}
            if self.report is not None:
                record["result"] = {
                    "outcome": self.report.outcome,
                    "total": len(self.report.checks),
                    "failures": sum(not ok for ok, _ in self.report.checks)}
            return record

    class StubPublisher:
        def __init__(self, run):
            self.run = run

        def begin(self, token, owner, artifact):
            return self.run

        def cleanup(self):
            pass

    def run_argv(self, **extra):
        argv = ["run", "--build", self.build.path, "--target", "SherclawkInspectCheck",
                "--token", "t", "--owner", "alice", "--json"]
        for key, value in extra.items():
            argv += ["--" + key.replace("_", "-"), value]
        return argv

    def test_run_publishes_fixtures_then_the_request(self):
        with tempfile.TemporaryDirectory() as tmp:
            scenario = os.path.join(tmp, "scenario.txt")
            with open(scenario, "wb") as f:
                f.write(b"pass\r")
            run = self.StubRun(report=contract.Report(
                contract.Request("slot1", "b" * 32, "c" * 32, "a" * 32,
                                 "SherclawkInspectCheck",
                                 "Retro68:Harness:slot1-g1:aaaa:SherclawkInspectCheck",
                                 "Retro68:Harness:slot1-g1:aaaa:", 10),
                "passed", [(True, "check")]))
            code, out, _err = self.invoke(
                self.run_argv(fixture="scenario=" + scenario),
                publisher=self.StubPublisher(run))
        self.assertEqual(code, 0)
        self.assertEqual(run.calls[0], "application")
        self.assertEqual(run.calls[1], ("fixture", "scenario", b"pass\r"))
        self.assertEqual(run.calls[2], ("request", publish.REQUEST_SECONDS))
        self.assertEqual(json.loads(out)["result"]["outcome"], "passed")

    def test_run_validates_fixtures_and_seconds_before_publishing(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "f.txt")
            with open(path, "wb") as f:
                f.write(b"x")
            for argv in (self.run_argv(fixture="my:file=" + path),
                         self.run_argv(fixture="a/b=" + path),
                         self.run_argv(fixture="SherclawkInspectCheck=" + path),
                         self.run_argv(seconds="0"),
                         self.run_argv(seconds="601")):
                run = self.StubRun()
                code, _out, err = self.invoke(argv, publisher=self.StubPublisher(run))
                self.assertEqual(code, 2, err)
                self.assertEqual(run.calls, [])
            argv = self.run_argv(fixture="a=" + path) + ["--fixture", "a=" + path]
            run = self.StubRun()
            code, _out, _err = self.invoke(argv, publisher=self.StubPublisher(run))
            self.assertEqual(code, 2)

    def test_failures_after_begin_name_the_retained_evidence(self):
        run = self.StubRun(error=publish.RemoteError("ssh failed"))
        _code, _out, err = self.invoke(self.run_argv(), publisher=self.StubPublisher(run))
        self.assertIn("evidence retained under /tmp/evidence", err)

    def test_install_must_be_named_for_the_target(self):
        build = BuildTree("Sherclawk")
        self.addCleanup(build.close)
        remote = FakeRemote()
        code, _out, err = self.invoke(
            ["install", "--build", build.path, "--target", "Sherclawk",
             "--share-host", "share-host", "--dest", "/srv/retro68/Other"],
            remote=remote)
        self.assertEqual(code, 2)
        self.assertIn("must be named Sherclawk", err)
        self.assertEqual(remote.ops(), [])

    def test_run_maps_lease_publication_and_result_outcomes(self):
        run = self.StubRun(error=reservation.LeaseError("lease expired"))
        code, _out, err = self.invoke(self.run_argv(), publisher=self.StubPublisher(run))
        self.assertEqual(code, 2)
        self.assertIn("lease expired", err)
        run = self.StubRun(error=publish.RemoteError("ssh failed"))
        code, _out, err = self.invoke(self.run_argv(), publisher=self.StubPublisher(run))
        self.assertEqual(code, 3)
        self.assertIn("ssh failed", err)
        report = contract.Report(
            contract.Request("slot1", "b" * 32, "c" * 32, "a" * 32, "SherclawkInspectCheck",
                             "Retro68:Harness:slot1-g1:aaaa:SherclawkInspectCheck",
                             "Retro68:Harness:slot1-g1:aaaa:", 10),
            "failed", [(False, "broken")])
        code, out, _err = self.invoke(self.run_argv(),
                                      publisher=self.StubPublisher(self.StubRun(report=report)))
        self.assertEqual(code, 1)
        self.assertEqual(json.loads(out)["result"]["outcome"], "failed")


if __name__ == "__main__":
    unittest.main()
