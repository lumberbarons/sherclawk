"""Host tests for exclusive slot reservations; no QEMU, VM or network."""
import contextlib
import io
import json
import os
import sys
import tempfile
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import manage  # noqa: E402
import provision  # noqa: E402
import qmp  # noqa: E402
import reservation  # noqa: E402


class FakeClock(reservation.Clock):
    def __init__(self, monotonic=1000.0, wall=1_700_000_000.0):
        self.mono = monotonic
        self.wall_time = wall

    def monotonic(self):
        return self.mono

    def wall(self):
        return self.wall_time

    def sleep(self, seconds):
        self.mono += seconds
        self.wall_time += seconds

    def advance(self, seconds, mono=True, wall=True):
        if mono:
            self.mono += seconds
        if wall:
            self.wall_time += seconds


class FakeQMP:
    def __init__(self, record):
        self.record = record
        self.closed = False

    def screendump(self, path):
        self.record.append(("screendump", path))
        with open(path, "wb") as f:
            f.write(b"P6\n1 1\n255\n\x00\x00\x00")

    def use_relative_mouse(self):
        self.record.append(("relative",))

    def click(self):
        self.record.append(("click",))

    def key(self, *codes):
        self.record.append(("key",) + codes)

    def type_text(self, text):
        self.record.append(("type", text))

    def close(self):
        self.closed = True


class FakeEffects(reservation.Effects):
    """Effects with a deterministic process table and no QEMU or share I/O."""

    def __init__(self, home, share, clock):
        super().__init__(home, share=share, clock=clock)
        self.host_boot = "boot-a"
        self.lock = threading.RLock()
        self.running = {}
        self.pidfiles = {}
        self.next_pid = 4000
        self.boot_seq = 0
        self.launched = []
        self.terminated = []
        self.resets = []
        self.qmp_calls = []
        self.command_overrides = {}
        self.ready_error = None
        self.die_on_ready = False
        self.launch_error = None
        self.reset_error = None
        self.terminate_error = None
        self.stubborn = False
        self.qmp_error = None
        self.qmp_live = False
        self.clear_error = None

    def boot_id(self):
        return self.host_boot

    def pidfile_pid(self, slot):
        return self.pidfiles.get(slot.name)

    def process(self, pid):
        with self.lock:
            if pid not in self.running:
                return reservation.ProcessInfo(pid, False, "")
            run = self.running[pid]
            command = self.command_overrides.get(
                pid, "/opt/homebrew/bin/qemu-system-ppc -drive file=%s,format=qcow2"
                % run["overlay"])
            return reservation.ProcessInfo(pid, True, command)

    def terminate(self, pid, timeout=reservation.STOP_TIMEOUT):
        with self.lock:
            self.terminated.append(pid)
            if self.terminate_error:
                raise reservation.RecoveryError(str(self.terminate_error))
            if self.stubborn:
                raise reservation.RecoveryError("pid %d ignored SIGTERM" % pid)
            run = self.running.pop(pid, None)
            if run:
                self.pidfiles.pop(run["slot"], None)

    def ensure_overlay(self, slot):
        self.launched.append(("overlay", slot.name))

    def launch(self, slot):
        with self.lock:
            if self.launch_error:
                raise reservation.ReservationError(str(self.launch_error))
            self.next_pid += 1
            pid = self.next_pid
            self.running[pid] = {"slot": slot.name, "overlay": slot.overlay}
            self.pidfiles[slot.name] = pid
            self.launched.append((slot.name, pid))
            return pid

    def reset(self, slot):
        with self.lock:
            self.resets.append(slot.name)
            if self.reset_error:
                raise reservation.RecoveryError(str(self.reset_error))
            self.pidfiles.pop(slot.name, None)

    def wait_ready(self, slot, previous_boot=None, timeout=reservation.READY_TIMEOUT):
        with self.lock:
            if self.ready_error:
                raise reservation.ReservationError(str(self.ready_error))
            if self.die_on_ready:
                name = slot if isinstance(slot, str) else slot.name
                for pid, run in list(self.running.items()):
                    if run["slot"] == name:
                        self.running.pop(pid)
                        self.pidfiles.pop(name, None)
                raise reservation.ReservationError("guest exited before readiness")
            self.boot_seq += 1
            return ("%032x" % self.boot_seq, int(self.clock.monotonic()))

    def open_qmp(self, slot, timeout=None, capture_dir=None):
        if self.qmp_error:
            raise qmp.QMPError(str(self.qmp_error))
        self.qmp_calls.append((slot.name, capture_dir))
        return FakeQMP(self.qmp_calls)

    def qmp_alive(self, slot, timeout=2.0):
        return self.qmp_live

    def clear_stale_readiness(self, slot):
        if self.clear_error:
            raise reservation.ConfigError(str(self.clear_error))
        path = os.path.join(self.mailbox(slot), "ready.tmp")
        try:
            os.remove(path)
        except FileNotFoundError:
            return False
        return True


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = provision.Home(os.path.join(self.tmp.name, "state"))
        self.share = os.path.join(self.tmp.name, "share")
        for name in provision.SLOTS:
            os.makedirs(os.path.join(self.share, "Harness", name))
        self.clock = FakeClock()
        self.effects = FakeEffects(self.home, self.share, self.clock)
        self.manager = self.new_manager()

    def new_manager(self, **kw):
        kw.setdefault("home", self.home)
        kw.setdefault("clock", self.clock)
        kw.setdefault("effects", self.effects)
        return reservation.Manager(**kw)


class ReservationTest(Base):
    def test_two_reserves_get_distinct_slots_and_a_third_is_refused(self):
        a = self.manager.reserve("alice")
        b = self.manager.reserve("bob")
        self.assertEqual(a.slot, "slot1")
        self.assertEqual(b.slot, "slot2")
        self.assertNotEqual(a.token, b.token)
        self.assertEqual((a.generation, b.generation), (1, 1))
        self.assertEqual(len(a.token), 32)
        with self.assertRaises(reservation.NoCapacity) as caught:
            self.manager.reserve("carol")
        self.assertIn("slot1=active", str(caught.exception))
        self.assertIn("slot2=active", str(caught.exception))

    def test_concurrent_reserves_grant_each_slot_to_one_owner(self):
        barrier = threading.Barrier(6)

        def attempt(index):
            manager = self.new_manager()
            barrier.wait()
            try:
                return manager.reserve("owner-%d" % index)
            except reservation.NoCapacity:
                return None

        with ThreadPoolExecutor(max_workers=6) as pool:
            leases = list(pool.map(attempt, range(6)))
        held = [lease for lease in leases if lease]
        self.assertEqual(len(held), 2)
        self.assertEqual({lease.slot for lease in held}, {"slot1", "slot2"})
        self.assertEqual(len({lease.owner for lease in held}), 2)

    def test_stale_token_cannot_touch_a_new_owner(self):
        a = self.manager.reserve("alice")
        self.manager.release(a.token, "alice")
        b = self.manager.reserve("bob")
        self.assertEqual(b.slot, a.slot)
        self.assertGreater(b.generation, a.generation)
        for operation in (
                lambda: self.manager.renew(a.token, "alice"),
                lambda: self.manager.screenshot(a.token, "alice"),
                lambda: self.manager.input(a.token, "alice", text="x")):
            with self.assertRaises(reservation.LeaseError):
                operation()
        self.assertTrue(self.effects.process(b.pid).alive)
        self.assertNotIn(b.pid, self.effects.terminated)
        again = self.manager.release(a.token, "alice")
        self.assertTrue(again["already_released"])
        self.assertTrue(self.effects.process(b.pid).alive)

    def test_release_is_idempotent_and_does_not_stop_the_new_owner(self):
        a = self.manager.reserve("alice")
        first = self.manager.release(a.token, "alice")
        self.assertTrue(first["stopped"])
        self.assertTrue(first["reset"])
        self.assertFalse(self.effects.process(a.pid).alive)
        b = self.manager.reserve("bob")
        second = self.manager.release(a.token, "alice")
        self.assertTrue(second["already_released"])
        self.assertTrue(self.effects.process(b.pid).alive)
        self.assertNotIn(b.pid, self.effects.terminated)

    def test_expiry_revokes_and_recovers_before_reassignment(self):
        a = self.manager.reserve("alice", ttl=10)
        self.clock.advance(11)
        with self.assertRaisesRegex(reservation.LeaseError, "expired"):
            self.manager.renew(a.token, "alice")
        b = self.manager.reserve("bob", ttl=10)
        self.assertEqual(b.slot, a.slot)
        self.assertGreater(b.generation, a.generation)
        self.assertFalse(self.effects.process(a.pid).alive)
        self.assertIn(a.pid, self.effects.terminated)
        self.assertIn(a.slot, self.effects.resets)

    def test_host_sleep_advances_the_wall_deadline(self):
        a = self.manager.reserve("alice", ttl=600)
        self.clock.advance(700, mono=False, wall=True)
        with self.assertRaisesRegex(reservation.LeaseError, "expired"):
            self.manager.renew(a.token, "alice")
        self.assertEqual(self.manager.status()["slots"][0]["state"], "recovering")

    def test_expired_guest_operation_marks_the_lease_recovering(self):
        a = self.manager.reserve("alice", ttl=10)
        self.clock.advance(11)
        with self.assertRaisesRegex(reservation.LeaseError, "expired"):
            self.manager.screenshot(a.token, "alice")
        self.assertEqual(self.manager.status()["slots"][0]["state"], "recovering")

    def test_manager_restart_invalidates_every_lease_and_recovers(self):
        a = self.manager.reserve("alice")
        b = self.manager.reserve("bob")
        pids = {a.pid, b.pid}
        self.effects.host_boot = "boot-b"
        restarted = self.new_manager()
        for token, owner in ((a.token, "alice"), (b.token, "bob")):
            with self.assertRaises(reservation.LeaseError):
                restarted.renew(token, owner)
            with self.assertRaises(reservation.LeaseError):
                restarted.screenshot(token, owner)
        for pid in pids:
            self.assertFalse(self.effects.process(pid).alive)
        self.assertEqual([s["state"] for s in restarted.status()["slots"]], ["free", "free"])
        c = restarted.reserve("carol")
        self.assertGreater(c.generation, 1)

    def test_launch_without_readiness_quarantines_the_slot(self):
        self.effects.ready_error = "no controller heartbeat"
        with self.assertRaises(reservation.ReservationError):
            self.manager.reserve("alice")
        status = self.manager.status()
        self.assertEqual(status["slots"][0]["state"], "quarantined")
        self.assertIn("no controller heartbeat", status["slots"][0]["quarantine"])
        self.assertEqual(status["slots"][1]["state"], "free")
        self.effects.ready_error = None
        self.manager.reserve("bob")
        with self.assertRaises(reservation.NoCapacity):
            self.manager.reserve("carol")

    def test_guest_exit_before_readiness_quarantines_the_slot(self):
        self.effects.die_on_ready = True
        with self.assertRaisesRegex(reservation.ReservationError, "before readiness"):
            self.manager.reserve("alice")
        self.assertEqual(self.manager.status()["slots"][0]["state"], "quarantined")

    def test_stale_readiness_staging_is_cleared_before_boot(self):
        path = os.path.join(self.share, "Harness", "slot1", "ready.tmp")
        with open(path, "wb") as f:
            f.write(b"truncated")
        self.manager.reserve("alice")
        self.assertFalse(os.path.exists(path))

    def test_unwritable_stale_readiness_quarantines_the_slot(self):
        self.effects.clear_error = "read-only share"
        with self.assertRaisesRegex(reservation.ConfigError, "read-only share"):
            self.manager.reserve("alice")
        self.assertEqual(self.manager.status()["slots"][0]["state"], "quarantined")

    def test_live_untracked_qmp_is_never_reused(self):
        self.effects.qmp_live = True
        with self.assertRaisesRegex(reservation.ReservationError, "live QMP socket"):
            self.manager.reserve("alice")
        self.assertEqual(self.manager.status()["slots"][0]["state"], "quarantined")
        summary = self.manager.recover("slot1")
        self.assertTrue(summary["quarantined"])
        self.assertEqual(self.manager.status()["slots"][0]["state"], "quarantined")

    def test_expired_launching_row_is_recovered_before_reassignment(self):
        a = self.manager.reserve("alice")
        mono, wall = self.manager._now()
        with self.manager._txn() as conn:
            conn.execute("""UPDATE slots SET state='launching', expires_mono=?,
                            expires_wall=? WHERE slot=?""",
                         (mono - 1, wall - 1, a.slot))
        b = self.manager.reserve("bob")
        self.assertEqual(b.slot, a.slot)
        self.assertGreater(b.generation, a.generation)
        self.assertFalse(self.effects.process(a.pid).alive)
        self.assertIn(a.slot, self.effects.resets)

    def test_hung_qmp_is_evidence_only_and_does_not_block_release(self):
        a = self.manager.reserve("alice")
        self.effects.qmp_error = "QMP read timed out"
        summary = self.manager.release(a.token, "alice")
        self.assertFalse(summary["quarantined"])
        self.assertTrue(summary["stopped"])
        self.assertTrue(summary["reset"])
        with open(summary["evidence"][0]) as f:
            evidence = json.load(f)
        self.assertIn("QMP read timed out", evidence["screenshot_error"])

    def test_explicit_restart_recovers_a_lease_without_a_host_reboot(self):
        a = self.manager.reserve("alice")
        restarted = self.new_manager()
        result = restarted.recover()
        self.assertTrue(result["restarted"])
        with self.assertRaises(reservation.LeaseError):
            restarted.renew(a.token, "alice")
        self.assertFalse(self.effects.process(a.pid).alive)
        self.assertEqual(restarted.status()["slots"][0]["state"], "free")

    def test_failed_reset_quarantines_until_explicit_recovery(self):
        a = self.manager.reserve("alice")
        self.effects.reset_error = "overlay is busy"
        summary = self.manager.release(a.token, "alice")
        self.assertTrue(summary["quarantined"])
        self.assertFalse(summary["reset"])
        self.assertEqual(self.manager.status()["slots"][0]["state"], "quarantined")
        self.effects.reset_error = None
        recovered = self.manager.recover("slot1")
        self.assertTrue(recovered["reset"])
        self.assertEqual(self.manager.status()["slots"][0]["state"], "free")

    def test_a_foreign_process_is_never_stopped(self):
        a = self.manager.reserve("alice")
        self.effects.command_overrides[a.pid] = "/usr/bin/sleep 999"
        summary = self.manager.release(a.token, "alice")
        self.assertTrue(summary["quarantined"])
        self.assertIn("not slot1's QEMU", summary["error"])
        self.assertNotIn(a.pid, self.effects.terminated)
        self.assertTrue(self.effects.process(a.pid).alive)

    def test_a_process_that_ignores_sigterm_quarantines_the_slot(self):
        a = self.manager.reserve("alice")
        self.effects.stubborn = True
        summary = self.manager.release(a.token, "alice")
        self.assertTrue(summary["quarantined"])
        self.assertIn(a.pid, self.effects.terminated)
        self.assertTrue(self.effects.process(a.pid).alive)

    def test_process_identity_tolerates_ps_column_spacing(self):
        slot = provision.slot(self.home, "slot1")
        info = reservation.ProcessInfo(1, True,
                                       "   qemu-system-ppc -drive file=%s,format=qcow2"
                                       % slot.overlay)
        self.assertTrue(self.manager._is_our_qemu(slot, info))
        self.assertFalse(self.manager._is_our_qemu(
            slot, reservation.ProcessInfo(1, True, "qemu-system-ppc -drive file=/elsewhere")))

    def test_release_collects_evidence_and_a_screenshot(self):
        a = self.manager.reserve("alice")
        summary = self.manager.release(a.token, "alice")
        self.assertEqual(len(summary["evidence"]), 1)
        path = summary["evidence"][0]
        self.assertTrue(os.path.exists(path))
        with open(path) as f:
            data = json.load(f)
        self.assertEqual(data["generation"], a.generation)
        self.assertEqual(data["owner"], "alice")
        self.assertEqual(data["reason"], "released")
        self.assertIn("screenshot", data)
        self.assertTrue(os.path.exists(data["screenshot"]))
        self.assertTrue(any(call[0] == "screendump" for call in self.effects.qmp_calls))

    def test_screenshot_and_input_are_lease_checked(self):
        a = self.manager.reserve("alice")
        out = os.path.join(self.tmp.name, "shot.ppm")
        self.assertEqual(self.manager.screenshot(a.token, "alice", out)["path"], out)
        self.manager.input(a.token, "alice", text="ok", click=True)
        calls = self.effects.qmp_calls
        self.assertTrue(any(call[0] == "screendump" for call in calls))
        self.assertTrue(any(call[0] == "type" and call[1] == "ok" for call in calls))
        with self.assertRaises(reservation.LeaseError):
            self.manager.input(a.token, "mallory", text="no")

    def test_status_reports_expired_and_untracked_guests(self):
        a = self.manager.reserve("alice", ttl=10)
        self.clock.advance(11)
        slot = self.manager.status()["slots"][0]
        self.assertEqual(slot["state"], "expired")
        self.assertTrue(slot["pid_alive"])
        self.assertEqual(slot["owner"], "alice")
        self.manager.release(a.token, "alice")

    def test_run_renews_during_a_bounded_command(self):
        clock = reservation.Clock()
        home = provision.Home(os.path.join(self.tmp.name, "run-state"))
        effects = FakeEffects(home, self.share, clock)
        manager = reservation.Manager(home=home, clock=clock, effects=effects,
                                      renew_interval=0.05)
        lease = manager.reserve("alice")
        calls = []
        original = manager.renew

        def renew(*args, **kw):
            calls.append(args)
            return original(*args, **kw)

        manager.renew = renew
        code = manager.run(lease.token, "alice",
                           [sys.executable, "-c", "import time; time.sleep(0.2)"],
                           timeout=5)
        self.assertEqual(code, 0)
        self.assertGreaterEqual(len(calls), 2)

    def test_suite_stops_at_the_first_failing_step(self):
        lease = self.manager.reserve("alice")
        result = self.manager.suite(lease.token, "alice", [
            ("first", [sys.executable, "-c", "pass"], 5),
            ("second", [sys.executable, "-c", "raise SystemExit(3)"], 5),
            ("third", [sys.executable, "-c", "raise SystemExit(4)"], 5),
        ])
        self.assertFalse(result["passed"])
        self.assertEqual([step["name"] for step in result["steps"]], ["first", "second"])


class CliTest(unittest.TestCase):
    class Stub:
        def __init__(self):
            self.clock = FakeClock()
            self.calls = []

        def reserve(self, owner, ttl):
            return reservation.Lease("slot1", "a" * 32, owner, 1,
                                     1010.0, 1_700_000_010.0, "b" * 32, 300, 4000, "active")

        def renew(self, token, owner, ttl):
            raise reservation.LeaseError("lease expired")

        def run(self, token, owner, argv, timeout):
            self.calls.append(argv)
            return 0

        def suite(self, token, owner, steps, stop_on_failure):
            self.calls.append(steps)
            return {"steps": [{"name": name, "command": argv, "returncode": 0}
                              for name, argv, _timeout in steps], "passed": True}

    def invoke(self, stub, argv):
        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(reservation, "Manager", return_value=stub), \
                contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = manage.main(argv)
        return code, out.getvalue(), err.getvalue()

    def test_reserve_json_prints_the_token(self):
        stub = self.Stub()
        code, out, _err = self.invoke(stub, ["--json", "reserve", "--owner", "alice"])
        self.assertEqual(code, 0)
        data = json.loads(out)
        self.assertEqual(data["slot"], "slot1")
        self.assertEqual(data["token"], "a" * 32)
        self.assertEqual(data["owner"], "alice")

    def test_stale_renew_exits_two(self):
        code, _out, err = self.invoke(self.Stub(), ["renew", "t", "--owner", "alice"])
        self.assertEqual(code, 2)
        self.assertIn("lease expired", err)

    def test_run_strips_the_separator_and_returns_the_child_status(self):
        stub = self.Stub()
        code, _out, _err = self.invoke(
            stub, ["run", "t", "--owner", "alice", "--", sys.executable, "-c", "x"])
        self.assertEqual(code, 0)
        self.assertEqual(stub.calls, [[sys.executable, "-c", "x"]])

    def test_suite_steps_are_shell_split(self):
        stub = self.Stub()
        code, out, _err = self.invoke(
            stub, ["suite", "t", "--owner", "alice", "--step", "one::echo hi"])
        self.assertEqual(code, 0)
        self.assertIn("suite passed", out)
        self.assertEqual(stub.calls[0][0][1], ["echo", "hi"])


if __name__ == "__main__":
    unittest.main()
