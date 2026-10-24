"""Unattended suite contract: registry, serial runs, reset policy, evidence.

No QEMU, share host or guest: a fake publisher stands in for publication and
the reservation manager runs against its fake effects, so only the suite's own
decisions and records are under test.
"""
import contextlib
import dataclasses
import io
import json
import os
import secrets
import stat
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import contract  # noqa: E402
import publish  # noqa: E402
import reservation  # noqa: E402
import suite  # noqa: E402
from test_reservation import FakeClock  # noqa: E402
from test_publish import BuildTree, FakeRemote, ReadyEffects  # noqa: E402


def check(name="write", target="SherclawkWriteCheck", **kw):
    entry = {"name": name, "target": target, "timeout": 120}
    entry.update(kw)
    return entry


def write_registry(directory, checks, schema=1, raw=None):
    path = os.path.join(directory, "registry.json")
    with open(path, "w", encoding="utf-8") as f:
        if raw is not None:
            f.write(raw)
        else:
            json.dump({"schema": schema, "checks": checks}, f)
    return path


@dataclasses.dataclass
class Artifact:
    target: str


class FakeRun:
    """One publication run whose outcome is chosen by the test."""

    def __init__(self, publisher, lease, artifact, spec):
        self.publisher = publisher
        self.lease = lease
        self.artifact = artifact
        self.spec = dict(spec)
        self.run_id = secrets.token_hex(16)
        self.workspace = "Retro68:Harness:%s-g%d:%s:" % (
            lease.slot, lease.generation, self.run_id[:20])
        self.application = self.workspace + artifact.target
        self.mailbox_share = os.path.join(publisher.share, "Harness", lease.slot)
        self.evidence_dir = os.path.join(publisher.runs, self.run_id)
        self.evidence_path = os.path.join(self.evidence_dir, "evidence.json")
        os.makedirs(self.evidence_dir)
        self.calls = []
        self.request = None
        self.state = "begun"
        self.report = None
        self._evidence()

    def _evidence(self):
        with open(self.evidence_path, "w", encoding="utf-8") as f:
            if "evidence_raw" in self.spec:
                f.write(self.spec["evidence_raw"])
            else:
                json.dump({"phase": self.state}, f)

    def _fail(self, phase):
        if self.spec.get("fail") == phase:
            raise publish.PublicationError("%s failed" % phase)

    def publish_application(self):
        self.calls.append("application")
        self._fail("application")

    def publish_fixture(self, name, data):
        self.calls.append(("fixture", name, data))
        self._fail("fixture")

    def publish_request(self, seconds=120):
        self.calls.append(("request", seconds))
        self._fail("request")
        if self.spec.get("leave_request"):
            os.makedirs(self.mailbox_share, exist_ok=True)
            with open(os.path.join(self.mailbox_share, "request"), "wb") as f:
                f.write(b"unconsumed")
        self.request = contract.Request(
            self.lease.slot, "%032x" % self.lease.generation, self.lease.boot,
            self.run_id, self.artifact.target, self.application, self.workspace,
            (self.lease.boot_clock or 4000000000) + seconds)
        with open(os.path.join(self.evidence_dir, "request.bin"), "wb") as f:
            f.write(contract.write_request(self.request))
        self.state = "published"

    def result_path(self):
        return os.path.join(self.mailbox_share,
                            contract.filename(self.request, "result"))

    def collect(self, timeout=None, screenshot=True, on_poll=None):
        self.calls.append(("collect", timeout, screenshot))
        if on_poll is not None:
            on_poll()
        if self.publisher.on_collect is not None:
            self.publisher.on_collect(self)
        if "raise" in self.spec:
            raise self.spec["raise"]
        outcome = self.spec.get("outcome", "passed")
        checks = self.spec.get("checks", ((True, "one check"),))
        self.report = contract.Report(self.request, outcome, list(checks))
        self.state = "settled"
        self._evidence()
        return self.report


class FakePublisher:
    def __init__(self, share, runs, specs=()):
        self.share = share
        self.runs = runs
        self.specs = list(specs)
        self.begins = []
        self.on_collect = None

    def begin(self, token, owner, artifact):
        spec = self.specs.pop(0) if self.specs else {}
        if spec.get("fail") == "begin":
            raise publish.PublicationError("begin failed")
        run = FakeRun(self, self._lease(token, owner), artifact, spec)
        self.begins.append(run)
        return run

    def _lease(self, token, owner):
        return self.manager.lease(token, owner)


class SuiteBase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = os.path.join(self.tmp.name, "state")
        self.share = os.path.join(self.tmp.name, "share")
        os.makedirs(os.path.join(self.share, "Harness"))
        self.clock = FakeClock()
        self.effects = ReadyEffects(reservation.provision.Home(self.home),
                                    self.share, self.clock)
        self.manager = reservation.Manager(
            home=self.home, share=self.share, clock=self.clock,
            effects=self.effects)
        self.publisher = FakePublisher(self.share,
                                       os.path.join(self.tmp.name, "runs"))
        self.publisher.manager = self.manager

    def registry(self, entries):
        return suite.load_registry(write_registry(self.tmp.name, entries))

    def runner(self, **kw):
        kw.setdefault("home", self.home)
        kw.setdefault("share", self.share)
        kw.setdefault("manager", self.manager)
        kw.setdefault("publisher", self.publisher)
        kw.setdefault("clock", self.clock)
        kw.setdefault("evidence_dir", os.path.join(self.tmp.name, "suites"))
        return suite.Suite(**kw)

    @staticmethod
    def artifacts(*targets):
        return {target: Artifact(target) for target in targets}

    def run_suite(self, entries, specs=(), selected=None, keep_going=False):
        self.publisher.specs = list(specs)
        self.last_runner = self.runner()
        registry = self.registry(entries)
        targets = {entry["target"] for entry in entries}
        self.last_summary = self.last_runner.run(
            registry, owner="alice", artifacts=self.artifacts(*targets),
            selected=selected, keep_going=keep_going)
        return self.last_summary


class RegistryTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def load(self, entries, **kw):
        return suite.load_registry(write_registry(self.tmp.name, entries, **kw))

    def assert_bad(self, entries, text, **kw):
        with self.assertRaises(suite.RegistryError) as caught:
            self.load(entries, **kw)
        self.assertIn(text, str(caught.exception))

    def test_defaults_and_selection_keep_registry_order(self):
        registry = self.load([
            check("write", reset="reset"),
            check("inspect", target="SherclawkInspectCheck", timeout=300),
            check("scroll", target="SherclawkScrollCheck", expect="failed",
                  mode="visual"),
        ])
        self.assertEqual([c.name for c in registry.checks],
                         ["write", "inspect", "scroll"])
        self.assertEqual((registry.checks[0].expect, registry.checks[0].reset,
                          registry.checks[0].mode), ("passed", "reset",
                                                     "automated"))
        self.assertEqual([c.name for c in registry.select(["scroll", "write"])],
                         ["write", "scroll"])
        self.assertEqual(len(registry.digest), 64)
        self.assertEqual(registry.path, os.path.join(self.tmp.name, "registry.json"))

    def test_fixture_paths_are_resolved_next_to_the_registry(self):
        os.makedirs(os.path.join(self.tmp.name, "fixtures"))
        registry = self.load([check(fixtures={"scenario": "fixtures/scenario"})])
        self.assertEqual(registry.checks[0].fixtures,
                         (("scenario", os.path.join(self.tmp.name, "fixtures",
                                                    "scenario")),))

    def test_structural_errors_fail_before_anything_runs(self):
        self.assert_bad([], "non-empty")
        self.assert_bad([check()], "schema", schema=2)
        self.assert_bad([check(extra=1)], "unknown key")
        self.assert_bad([check(name="Two Words")], "not a slug")
        self.assert_bad([check(name="same"), check(name="same")], "repeats")
        self.assert_bad([check(target="SherclawkBuildCheck")], "not registered")
        for timeout in (0, 601, True, "120"):
            with self.subTest(timeout=timeout):
                self.assert_bad([check(timeout=timeout)], "timeout")
        self.assert_bad([check(expect="stopped")], "expect")
        self.assert_bad([check(reset="reboot")], "reset")
        self.assert_bad([check(mode="manual")], "mode")
        self.assert_bad([check(fixtures=[])], "fixtures must be an object")
        self.assert_bad([check(fixtures={"bad:name": "x"})], "single")
        self.assert_bad([check(fixtures={"SherclawkWriteCheck.log": "x"})],
                        "reserved")
        self.assert_bad([check(fixtures={"scenario": 1})], "needs a path")

    def test_schema_must_be_the_integer_or_string_one(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        for schema in (1, "1"):
            with self.subTest(schema=schema):
                suite.load_registry(
                    write_registry(tmp.name, [check()], schema=schema))
        for schema in (True, 1.0, 2, None, "one"):
            with self.subTest(schema=schema):
                with self.assertRaises(suite.RegistryError):
                    suite.load_registry(
                        write_registry(tmp.name, [check()], schema=schema))

    def test_unknown_selection_lists_known_checks(self):
        registry = self.load([check()])
        with self.assertRaises(suite.RegistryError) as caught:
            registry.select(["missing"])
        self.assertIn("unknown check", str(caught.exception))

    def test_invalid_json_and_missing_file_are_registry_errors(self):
        path = write_registry(self.tmp.name, [], raw="{")
        with self.assertRaises(suite.RegistryError):
            suite.load_registry(path)
        with self.assertRaises(suite.RegistryError):
            suite.load_registry(os.path.join(self.tmp.name, "absent.json"))


class RunTest(SuiteBase):
    def test_checks_run_serially_with_private_workspaces_and_evidence(self):
        summary = self.run_suite([
            check("write"),
            check("search", target="SherclawkSearchCheck"),
            check("inspect", target="SherclawkInspectCheck"),
        ])
        self.assertTrue(summary["passed"], summary)
        self.assertEqual(summary["phase"], "finished")
        self.assertEqual([step["name"] for step in summary["checks"]],
                         ["write", "search", "inspect"])
        self.assertEqual([run.artifact.target for run in self.publisher.begins],
                         ["SherclawkWriteCheck", "SherclawkSearchCheck",
                          "SherclawkInspectCheck"])
        workspaces = [step["run"]["workspace"] for step in summary["checks"]]
        self.assertEqual(len(workspaces), len(set(workspaces)))
        for step in summary["checks"]:
            self.assertTrue(os.path.isdir(step["run"]["evidence_dir"]))
            self.assertEqual(step["run"]["phase"], "settled")
        with open(os.path.join(summary["evidence_dir"], "suite.json"),
                  encoding="utf-8") as f:
            self.assertEqual(json.load(f)["passed"], True)
        status = self.manager.status()
        self.assertEqual([slot["state"] for slot in status["slots"]],
                         ["free", "free"])
        with open(os.path.join(summary["evidence_dir"], "session.json"),
                  encoding="utf-8") as f:
            session = json.load(f)
        self.assertFalse(session["active"])
        self.assertIsNone(session["token"])
        self.assertEqual(stat.S_IMODE(os.stat(os.path.join(
            summary["evidence_dir"], "session.json")).st_mode), 0o600)

    def test_expected_failure_is_a_completed_check(self):
        summary = self.run_suite([check(expect="failed")],
                                 specs=[{"outcome": "failed",
                                         "checks": ((False, "deliberate"),)}])
        self.assertTrue(summary["passed"])
        self.assertEqual(summary["checks"][0]["outcome"], "failed")
        self.assertTrue(summary["checks"][0]["passed"])

    def test_uncertain_terminal_outcomes_never_pass_or_replay(self):
        for outcome in ("failed", "unknown", "stopped", "harness_error"):
            with self.subTest(outcome=outcome):
                self.setUp()
                summary = self.run_suite([check()], specs=[{"outcome": outcome}])
                self.assertFalse(summary["passed"])
                self.assertEqual(summary["checks"][0]["outcome"], outcome)
                self.assertFalse(summary["checks"][0]["passed"])
                self.assertEqual(len(self.publisher.begins), 1)

    def test_missing_invalid_and_timed_out_reports_never_pass(self):
        failures = [
            publish.RunTimeout("no closed result"),
            publish.PublicationError("invalid result: bad framing"),
            contract.ContractError("mismatched identity"),
        ]
        for raised in failures:
            with self.subTest(raised=type(raised).__name__):
                self.setUp()
                summary = self.run_suite([check()], specs=[{"raise": raised}])
                expected = ("unknown" if isinstance(raised, publish.RunTimeout)
                            else "invalid_evidence"
                            if isinstance(raised, contract.ContractError)
                            else "error")
                self.assertFalse(summary["passed"])
                self.assertEqual(summary["checks"][0]["outcome"], expected)
                self.assertEqual(len(self.publisher.begins), 1)

    def test_publication_failure_is_attributed_to_that_check(self):
        summary = self.run_suite([check()], specs=[{"fail": "request"}])
        self.assertFalse(summary["passed"])
        self.assertEqual(summary["checks"][0]["outcome"], "error")
        self.assertIn("request failed", summary["checks"][0]["error"])
        self.assertEqual(len(self.publisher.begins), 1)

    def test_begin_failure_is_a_failed_check_not_a_suite_abort(self):
        summary = self.run_suite([
            check("write"), check("search", target="SherclawkSearchCheck")],
            specs=[{"fail": "begin"}, {}], keep_going=True)
        self.assertFalse(summary["passed"])
        self.assertEqual(summary["checks"][0]["outcome"], "error")
        self.assertIn("begin failed", summary["checks"][0]["error"])
        self.assertIsNone(summary["checks"][0]["run"])
        self.assertIsNone(summary["current"])
        self.assertEqual(summary["checks"][1]["outcome"], "passed")

    def test_unreadable_or_odd_evidence_does_not_discard_a_finished_check(self):
        for raw in ("[]", "not json"):
            with self.subTest(raw=raw):
                summary = self.run_suite([check()],
                                         specs=[{"evidence_raw": raw}])
                step = summary["checks"][0]
                self.assertEqual(step["outcome"], "passed")
                self.assertIsNone(step["run"]["phase"])

    def test_session_stops_advertising_the_lease_while_it_is_released(self):
        seen = []
        real_release = self.manager.release

        def release(token, owner, *args, **kw):
            seen.append(suite.read_session(self.last_runner._session_path))
            return real_release(token, owner, *args, **kw)

        self.manager.release = release
        self.run_suite([
            check("write", reset="reset"),
            check("search", target="SherclawkSearchCheck")])
        self.assertEqual(len(seen), 2)
        for data in seen:
            self.assertFalse(data["active"])
            self.assertIsNone(data["token"])
            with self.assertRaises(suite.SessionError):
                suite.active_session(self.last_runner._session_path)

    def test_default_stops_at_the_first_failure(self):
        summary = self.run_suite([
            check("write"), check("search", target="SherclawkSearchCheck"),
            check("inspect", target="SherclawkInspectCheck")],
            specs=[{}, {"outcome": "failed"}, {}])
        self.assertFalse(summary["passed"])
        self.assertTrue(summary["stopped_early"])
        self.assertEqual([step["name"] for step in summary["checks"]],
                         ["write", "search"])
        self.assertEqual(len(self.publisher.begins), 2)

    def test_keep_going_resets_the_guest_after_a_failure(self):
        summary = self.run_suite([
            check("write"), check("search", target="SherclawkSearchCheck"),
            check("inspect", target="SherclawkInspectCheck")],
            specs=[{}, {"outcome": "failed"}, {}], keep_going=True)
        self.assertFalse(summary["passed"])
        self.assertEqual(len(summary["checks"]), 3)
        self.assertEqual(len(self.publisher.begins), 3)
        self.assertGreaterEqual(len(self.effects.resets), 1)
        self.assertEqual(len(summary["reservations"]), 2)
        self.assertNotEqual(summary["checks"][1]["run"]["generation"],
                            summary["checks"][2]["run"]["generation"])

    def test_reset_policy_starts_a_fresh_reservation(self):
        summary = self.run_suite([
            check("write", reset="reset"),
            check("search", target="SherclawkSearchCheck")])
        self.assertTrue(summary["passed"])
        self.assertEqual(len(summary["reservations"]), 2)
        self.assertGreaterEqual(len(self.effects.resets), 1)
        self.assertEqual(summary["checks"][0]["run"]["slot"],
                         summary["releases"][0]["slot"])
        self.assertNotEqual(summary["checks"][0]["run"]["generation"],
                            summary["checks"][1]["run"]["generation"])

    def test_artifacts_and_fixtures_are_validated_before_reserving(self):
        runner = self.runner()
        registry = self.registry([check()])
        with self.assertRaises(suite.RegistryError):
            runner.run(registry, owner="alice", build=None)
        self.assertEqual(self.manager.status()["manager"]["free"], 2)

        registry = self.registry([check(fixtures={"scenario": "missing"})])
        with self.assertRaises(suite.RegistryError):
            runner.run(registry, owner="alice",
                       artifacts=self.artifacts("SherclawkWriteCheck"))
        self.assertEqual(self.manager.status()["manager"]["free"], 2)

    def test_share_configuration_is_checked_before_reserving(self):
        publisher = publish.Publisher(home=self.home, share="",
                                      share_host="", manager=self.manager,
                                      clock=self.clock)
        runner = self.runner(publisher=publisher)
        registry = self.registry([check()])
        with self.assertRaises(publish.ConfigError):
            runner.run(registry, owner="alice",
                       artifacts=self.artifacts("SherclawkWriteCheck"))
        self.assertEqual(self.manager.status()["manager"]["free"], 2)

    def test_visual_checks_are_kept_out_of_the_unattended_suite(self):
        runner = self.runner()
        registry = self.registry([check(mode="visual")])
        with self.assertRaises(suite.RegistryError) as caught:
            runner.run(registry, owner="alice",
                       artifacts=self.artifacts("SherclawkWriteCheck"))
        self.assertIn("not automated", str(caught.exception))
        self.assertEqual(self.manager.status()["manager"]["free"], 2)

    def test_session_names_the_in_flight_check_and_request(self):
        seen = {}

        def inspect(run):
            with open(self.last_runner.summary["session"], encoding="utf-8") as f:
                seen.update(json.load(f))

        self.publisher.on_collect = inspect
        summary = self.run_suite([check()])
        self.assertTrue(summary["passed"])
        self.assertTrue(seen["active"])
        self.assertEqual(seen["check"]["name"], "write")
        self.assertEqual(seen["request"]["run"], summary["checks"][0]["run"]["id"])
        self.assertTrue(os.path.isfile(seen["request"]["path"]))

    def test_registry_digest_and_build_artifacts_can_come_from_the_build_tree(self):
        build = BuildTree("SherclawkWriteCheck")
        self.addCleanup(build.close)
        registry = self.registry([check()])
        runner = self.runner()
        summary = runner.run(registry, build=build.path, owner="alice")
        self.assertTrue(summary["passed"])
        self.assertEqual(summary["build"], os.path.abspath(build.path))
        self.assertEqual(runner.publisher.begins[0].artifact.target,
                         "SherclawkWriteCheck")

    def test_an_unconsumed_request_stops_the_suite_without_replay(self):
        summary = self.run_suite([
            check("write"), check("search", target="SherclawkSearchCheck")],
            specs=[{"leave_request": True}, {}], keep_going=True)
        self.assertFalse(summary["passed"])
        self.assertTrue(summary["stopped_early"])
        self.assertIn("unconsumed request", summary["error"])
        self.assertEqual(len(summary["checks"]), 1)
        self.assertEqual(len(self.publisher.begins), 1)

    def test_a_quarantined_release_fails_the_suite(self):
        self.effects.reset_error = "overlay is immutable"
        summary = self.run_suite([check()])
        self.assertFalse(summary["passed"])
        self.assertIn("quarantined", summary["error"])
        self.assertEqual(len(summary["releases"]), 1)
        self.assertTrue(summary["releases"][0]["result"]["quarantined"])

    def test_summary_links_the_registry_and_intervention_paths(self):
        summary = self.run_suite([check()])
        self.assertEqual(summary["registry"]["checks"], 1)
        self.assertEqual(len(summary["registry"]["sha256"]), 64)
        self.assertTrue(summary["interventions"].endswith(
            suite.INTERVENTIONS_NAME))
        self.assertTrue(summary["session"].endswith(suite.SESSION_NAME))
        self.assertTrue(os.path.isfile(summary["interventions"]))
        self.assertEqual(stat.S_IMODE(os.stat(summary["interventions"]).st_mode),
                         0o600)


class RealPublisherTest(unittest.TestCase):
    """The runner's use of publish.Run, beyond the fake publisher above."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = os.path.join(self.tmp.name, "state")
        self.share = os.path.join(self.tmp.name, "share")
        os.makedirs(os.path.join(self.share, "Harness"))
        self.clock = FakeClock()
        self.effects = ReadyEffects(reservation.provision.Home(self.home),
                                    self.share, self.clock)
        self.manager = reservation.Manager(home=self.home, share=self.share,
                                           clock=self.clock, effects=self.effects)
        self.remote = FakeRemote()
        self.publisher = publish.Publisher(
            home=self.home, share=self.share, share_host="share-host",
            remote=self.remote, manager=self.manager, clock=self.clock,
            wait_ready=self.read_heartbeat)
        self.build = BuildTree("SherclawkWriteCheck")
        self.addCleanup(self.build.close)

    def read_heartbeat(self, mailbox, slot):
        with open(os.path.join(mailbox, "ready"), "rb") as f:
            return contract.read_ready_clock(f.read(129), slot)

    def test_real_publication_runs_one_private_workspace_per_check(self):
        registry = suite.load_registry(write_registry(self.tmp.name, [
            {"name": "write", "target": "SherclawkWriteCheck", "timeout": 90},
            {"name": "inspect", "target": "SherclawkInspectCheck", "timeout": 120},
        ]))
        inspect = BuildTree("SherclawkInspectCheck")
        self.addCleanup(inspect.close)
        # One BuildTree directory cannot hold both targets; use a shared parent.
        build_dir = os.path.join(self.tmp.name, "build")
        os.makedirs(os.path.join(build_dir, ".rsrc"))
        for tree, target in ((self.build, "SherclawkWriteCheck"),
                             (inspect, "SherclawkInspectCheck")):
            for source, dest in (
                    (os.path.join(tree.path, target + ".APPL"),
                     os.path.join(build_dir, target + ".APPL")),
                    (os.path.join(tree.path, ".rsrc", target + ".APPL"),
                     os.path.join(build_dir, ".rsrc", target + ".APPL"))):
                with open(source, "rb") as f, open(dest, "wb") as g:
                    g.write(f.read())
        runner = suite.Suite(home=self.home, share=self.share,
                             manager=self.manager, publisher=self.publisher,
                             clock=self.clock,
                             evidence_dir=os.path.join(self.tmp.name, "suites"))
        seen = []

        def collect(run, timeout=None, screenshot=True, on_poll=None):
            if on_poll is not None:
                on_poll()
            # The controller renames the request to its claim before launching.
            os.rename(os.path.join(run.mailbox_share, "request"),
                      os.path.join(run.mailbox_share,
                                   contract.filename(run.request, "claim")))
            seen.append((run.request.diagnostic, timeout))
            report = contract.Report(run.request, "passed",
                                     [(True, "published check")])
            run.report = report
            run.state = "settled"
            run._write_evidence()
            return report

        with mock.patch.object(publish.Run, "collect", autospec=True,
                               side_effect=collect):
            summary = runner.run(registry, build_dir, owner="alice")
        self.assertTrue(summary["passed"], summary)
        self.assertEqual(seen, [("SherclawkWriteCheck", 105),
                                ("SherclawkInspectCheck", 135)])
        self.assertEqual(len({step["run"]["workspace"]
                              for step in summary["checks"]}), 2)
        self.assertEqual([step["run"]["phase"] for step in summary["checks"]],
                         ["settled", "settled"])
        self.assertTrue(os.path.exists(os.path.join(
            summary["checks"][0]["run"]["evidence_dir"], "request.bin")))
        self.assertEqual([slot["state"] for slot in
                          self.manager.status()["slots"]], ["free", "free"])


class LeaseKeeperTest(unittest.TestCase):
    def test_renews_only_after_its_interval_and_fails_with_a_lost_lease(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        home = os.path.join(tmp.name, "state")
        share = os.path.join(tmp.name, "share")
        os.makedirs(os.path.join(share, "Harness"))
        clock = FakeClock()
        effects = ReadyEffects(reservation.provision.Home(home), share, clock)
        manager = reservation.Manager(home=home, share=share, clock=clock,
                                      effects=effects)
        lease = manager.reserve("alice")
        keeper = suite.LeaseKeeper(manager, lease, interval=60)
        before = manager.lease(lease.token, "alice").expires_in(
            clock.monotonic(), clock.wall())
        clock.advance(30)
        keeper.tick()
        self.assertAlmostEqual(manager.lease(lease.token, "alice").expires_in(
            clock.monotonic(), clock.wall()), before - 30, places=3)
        clock.advance(31)
        keeper.tick()
        self.assertGreater(manager.lease(lease.token, "alice").expires_in(
            clock.monotonic(), clock.wall()), 550)
        manager.release(lease.token, "alice")
        clock.advance(61)
        with self.assertRaises(reservation.LeaseError):
            keeper.tick()


class InteractiveTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = os.path.join(self.tmp.name, "state")
        self.share = os.path.join(self.tmp.name, "share")
        os.makedirs(os.path.join(self.share, "Harness"))
        self.clock = FakeClock()
        self.effects = ReadyEffects(reservation.provision.Home(self.home),
                                    self.share, self.clock)
        self.manager = reservation.Manager(home=self.home, share=self.share,
                                           clock=self.clock, effects=self.effects)
        self.lease = self.manager.reserve("alice")
        self.directory = os.path.join(self.tmp.name, "suite")
        os.makedirs(self.directory)
        self.mailbox = os.path.join(self.share, "Harness", self.lease.slot)
        os.makedirs(self.mailbox, exist_ok=True)
        self.request = contract.Request(
            self.lease.slot, "%032x" % self.lease.generation, self.lease.boot,
            "a" * 32, "SherclawkWriteCheck",
            "Retro68:Harness:r:SherclawkWriteCheck",
            "Retro68:Harness:r:workspace:", self.lease.boot_clock + 120)
        self.request_path = os.path.join(self.directory, "request.bin")
        with open(self.request_path, "wb") as f:
            f.write(contract.write_request(self.request))
        self.session = {
            "schema": "1", "active": True, "suite": "s1", "owner": "alice",
            "token": self.lease.token, "slot": self.lease.slot,
            "generation": self.lease.generation, "boot": self.lease.boot,
            "mailbox": self.mailbox,
            "check": {"name": "write", "target": "SherclawkWriteCheck",
                      "run_id": self.request.run},
            "request": {"path": self.request_path, "run": self.request.run},
        }
        self.write_session()

    def write_session(self, **changes):
        self.session.update(changes)
        path = os.path.join(self.directory, "session.json")
        with open(path, "w", encoding="utf-8") as f:
            json.dump(self.session, f)
        os.chmod(path, 0o600)

    def interventions(self):
        path = os.path.join(self.directory, suite.INTERVENTIONS_NAME)
        with open(path, encoding="utf-8") as f:
            return [json.loads(line) for line in f if line.strip()]

    def test_stop_is_published_once_with_the_exact_request(self):
        result = suite.stop(self.directory, self.manager)
        path = os.path.join(self.mailbox,
                            contract.filename(self.request, "stop"))
        self.assertEqual(result["stop"], path)
        with open(path, "rb") as f:
            self.assertEqual(f.read(), contract.write_request(self.request))
        with self.assertRaises(suite.SessionError):
            suite.stop(self.directory, self.manager)
        self.assertEqual(self.interventions()[-1]["action"], "stop")

    def test_stop_refuses_a_request_from_another_generation(self):
        other = dataclasses.replace(self.request, generation="f" * 32)
        with open(self.request_path, "wb") as f:
            f.write(contract.write_request(other))
        with self.assertRaises(suite.SessionError):
            suite.stop(self.directory, self.manager)
        self.assertFalse(os.path.exists(os.path.join(
            self.mailbox, contract.filename(other, "stop"))))

    def test_stale_or_finished_sessions_are_refused(self):
        self.manager.release(self.lease.token, "alice")
        with self.assertRaises(reservation.LeaseError):
            suite.stop(self.directory, self.manager)
        self.write_session(active=False, token=None)
        with self.assertRaises(suite.SessionError):
            suite.screenshot(self.directory, self.manager, "shot.ppm")

    def test_screenshot_uses_the_live_lease_and_records_the_action(self):
        out = os.path.join(self.tmp.name, "shot.ppm")
        result = suite.screenshot(self.directory, self.manager, out)
        self.assertTrue(os.path.exists(result["path"]))
        self.assertEqual(self.interventions()[-1]["action"], "screenshot")

    def test_input_delegates_under_the_live_lease(self):
        with mock.patch.object(reservation.Manager, "input",
                               return_value={"typed": 2, "keys": 1,
                                             "clicked": True}) as typed:
            result = suite.input_text(self.directory, self.manager, "hi",
                                      ["return"], True)
        self.assertEqual(result["typed"], 2)
        typed.assert_called_once_with(self.lease.token, "alice", "hi",
                                      ["return"], True)
        self.assertEqual(self.interventions()[-1]["action"], "input")


class CliTest(unittest.TestCase):
    def test_validate_prints_the_registry_and_reports_bad_files(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = write_registry(tmp.name, [check()])
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(suite.main(["validate", "--registry", path]), 0)
        self.assertIn("checks=1", out.getvalue())
        self.assertIn("SherclawkWriteCheck", out.getvalue())
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(suite.main(["validate", "--registry",
                                         os.path.join(tmp.name, "absent")]), 2)
        self.assertIn("cannot read registry", err.getvalue())

    def test_run_failures_after_the_suite_began_exit_1_and_name_the_evidence(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = write_registry(tmp.name, [check()])

        class Lost:
            summary = {"evidence_dir": "/evidence/suite-1"}

            def __init__(self, **kw):
                pass

            def run(self, *args, **kw):
                raise reservation.LeaseError("lease expired")

        class Early(Lost):
            summary = None

        argv = ["run", "--registry", path, "--build", tmp.name,
                "--owner", "alice"]
        for runner, code, evidence in ((Lost, 1, True), (Early, 2, False)):
            with self.subTest(runner=runner.__name__):
                err = io.StringIO()
                with mock.patch.object(suite, "Suite", runner), \
                        contextlib.redirect_stderr(err):
                    self.assertEqual(suite.main(argv), code)
                self.assertIn("lease expired", err.getvalue())
                self.assertEqual("evidence retained under /evidence/suite-1"
                                 in err.getvalue(), evidence)

    def test_validate_reports_missing_artifacts_with_a_build(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = write_registry(tmp.name, [check()])
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            self.assertEqual(suite.main(["validate", "--registry", path,
                                         "--build", tmp.name]), 2)
        self.assertIn("no data fork", err.getvalue())


if __name__ == "__main__":
    unittest.main()
