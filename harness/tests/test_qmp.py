"""Host tests for the QMP client against a fake server: no QEMU or VM."""
import os
import shutil
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import fake_qmp  # noqa: E402
import qmp  # noqa: E402
import screen  # noqa: E402
from fake_qmp import Drop, Fail, Silent, ppm  # noqa: E402


class Sleeper:
    def __init__(self):
        self.calls = []

    def __call__(self, seconds):
        self.calls.append(seconds)


class ServerCase(unittest.TestCase):
    def setUp(self):
        self.server = fake_qmp.FakeQMP()
        self.addCleanup(self.server.close)
        self.work = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.work, True)
        self.sleep = Sleeper()

    def connect(self, **kw):
        kw.setdefault("timeout", 2)
        kw.setdefault("sleep", self.sleep)
        kw.setdefault("capture", qmp.Capture(os.path.join(self.work, "shots")))
        q = qmp.QMP(self.server.path, **kw)
        self.addCleanup(q.close)
        return q


class ProtocolTest(ServerCase):
    def test_responses_are_matched_by_id(self):
        self.server.on("query-status", lambda c, a: {"status": "running"})
        q = self.connect()
        self.assertEqual(q.cmd("query-status"), {"status": "running"})
        self.assertEqual(q.cmd("query-status"), {"status": "running"})
        ids = [self.server.last_id["qmp_capabilities"], self.server.last_id["query-status"]]
        self.assertEqual(ids, [1, 3])

    def test_events_before_the_response_are_recorded_and_skipped(self):
        def noisy(conn, _args):
            conn.send({"event": "RESET", "data": {"guest": True}})
            conn.send({"event": "STOP"})
            return {"ok": 1}
        self.server.on("noisy", noisy)
        q = self.connect()
        self.assertEqual(q.cmd("noisy"), {"ok": 1})
        self.assertEqual([e["event"] for e in q.events], ["RESET", "STOP"])
        self.assertEqual(q.resets, 1)

    def test_a_late_reply_to_an_abandoned_command_is_discarded(self):
        self.server.on("slow", lambda c, a: (_ for _ in ()).throw(Silent()))

        def next_cmd(conn, _args):
            conn.send({"return": "late", "id": self.server.last_id["slow"]})
            return "right"
        self.server.on("next", next_cmd)
        q = self.connect()
        with self.assertRaises(qmp.QMPTimeout):
            q.cmd("slow", timeout=0.2)
        self.assertEqual(q.cmd("next"), "right")

    def test_error_response_raises_with_the_description(self):
        def boom(_c, _a):
            raise Fail("no such thing")
        self.server.on("boom", boom)
        q = self.connect()
        with self.assertRaisesRegex(qmp.QMPCommandError, "no such thing"):
            q.cmd("boom")
        self.assertEqual(q.cmd("qmp_capabilities"), {})  # still usable

    def test_unknown_command_is_an_error(self):
        q = self.connect()
        with self.assertRaises(qmp.QMPCommandError):
            q.cmd("nonsense")

    def test_command_timeout_is_bounded(self):
        self.server.on("slow", lambda c, a: (_ for _ in ()).throw(Silent()))
        q = self.connect()
        t0 = time.monotonic()
        with self.assertRaises(qmp.QMPTimeout):
            q.cmd("slow", timeout=0.3)
        self.assertLess(time.monotonic() - t0, 1.5)

    def test_disconnect_mid_command_raises_and_stays_closed(self):
        self.server.on("die", lambda c, a: (_ for _ in ()).throw(Drop()))
        q = self.connect()
        with self.assertRaises(qmp.QMPDisconnected):
            q.cmd("die")
        with self.assertRaises(qmp.QMPDisconnected):
            q.cmd("qmp_capabilities")

    def test_greeting_timeout_names_the_likely_cause(self):
        self.server.greeting = None
        with self.assertRaisesRegex(qmp.QMPTimeout, "another process"):
            qmp.QMP(self.server.path, timeout=0.3)

    def test_non_qmp_greeting_is_refused(self):
        self.server.greeting = {"hello": "world"}
        with self.assertRaisesRegex(qmp.QMPError, "greeting"):
            qmp.QMP(self.server.path, timeout=1)

    def test_missing_socket_is_a_qmp_error(self):
        with self.assertRaisesRegex(qmp.QMPError, "cannot connect"):
            qmp.QMP(os.path.join(self.work, "none.sock"), timeout=1)


class InputTest(ServerCase):
    def test_key_holds_then_waits_out_the_release_gap(self):
        q = self.connect(key_hold_ms=200, key_gap=0.1)
        q.key("esc")
        (args,) = self.server.commands("send-key")
        self.assertEqual(args["hold-time"], 200)
        self.assertEqual(args["keys"], [{"type": "qcode", "data": "esc"}])
        self.assertGreaterEqual(self.sleep.calls[-1], 0.3)

    def test_every_key_in_a_string_is_followed_by_hold_plus_gap(self):
        q = self.connect(key_hold_ms=200, key_gap=0.1)
        q.type_text("aB-")
        sent = [[k["data"] for k in a["keys"]] for a in self.server.commands("send-key")]
        self.assertEqual(sent, [["a"], ["shift", "b"], ["minus"]])
        self.assertEqual(len(self.sleep.calls), 3)
        self.assertTrue(all(s >= 0.3 for s in self.sleep.calls))

    def test_text_with_an_unmappable_character_types_nothing(self):
        q = self.connect()
        with self.assertRaisesRegex(qmp.KeymapError, "offset 3"):
            q.type_text("abc\u00e9def")
        self.assertEqual(self.server.commands("send-key"), [])

    def test_plan_covers_punctuation_and_newline(self):
        self.assertEqual(qmp.plan_text("a:\n"), [("a",), ("shift", "semicolon"), ("ret",)])

    def test_move_sends_only_nonzero_axes(self):
        q = self.connect()
        q.move(5, 0)
        q.move(0, 0)
        (args,) = self.server.commands("input-send-event")
        self.assertEqual(args["events"], [{"type": "rel", "data": {"axis": "x", "value": 5}}])

    def test_click_presses_then_releases(self):
        q = self.connect()
        q.click()
        downs = [a["events"][0]["data"]["down"] for a in self.server.commands("input-send-event")]
        self.assertEqual(downs, [True, False])

    def test_click_releases_the_button_when_the_hold_is_interrupted(self):
        def interrupted(_seconds):
            raise KeyboardInterrupt
        q = self.connect(sleep=interrupted)
        with self.assertRaises(KeyboardInterrupt):
            q.click()
        downs = [a["events"][0]["data"]["down"] for a in self.server.commands("input-send-event")]
        self.assertEqual(downs, [True, False])

    def test_click_attempts_release_when_the_press_fails(self):
        calls = []

        def press_fails(_c, args):
            calls.append(args)
            if len(calls) == 1:
                raise Silent()
            return {}
        self.server.on("input-send-event", press_fails)
        q = self.connect(timeout=0.3)
        with self.assertRaises(qmp.QMPTimeout):
            q.click()
        self.assertEqual([c["events"][0]["data"]["down"] for c in calls], [True, False])


class MouseDiscoveryTest(ServerCase):
    def test_current_relative_device_is_left_alone(self):
        self.server.on("query-mice", lambda c, a: [
            {"name": "QEMU PS/2 Mouse", "index": 0, "current": True, "absolute": False},
            {"name": "QEMU HID Tablet", "index": 1, "current": False, "absolute": True}])
        q = self.connect()
        self.assertEqual(q.use_relative_mouse()["index"], 0)
        self.assertEqual(self.server.hmp_log, [])

    def test_switches_from_an_absolute_tablet_to_a_discovered_relative_device(self):
        state = {"current": 1}
        mice = [
            {"name": "QEMU HID Tablet", "index": 1, "absolute": True},
            {"name": "QEMU PS/2 Mouse", "index": 2, "absolute": False},
            {"name": "QEMU HID Mouse", "index": 7, "absolute": False}]
        self.server.on("query-mice", lambda c, a: [dict(m, current=m["index"] == state["current"])
                                                  for m in mice])

        def mouse_set(_c, line):
            state["current"] = int(line.split()[1])
            return ""
        self.server.on_hmp("mouse_set", mouse_set)
        q = self.connect()
        self.assertEqual(q.use_relative_mouse()["index"], 7)
        self.assertEqual(self.server.hmp_log, ["mouse_set 7"])

    def test_an_empty_device_list_is_not_an_error(self):
        self.server.on("query-mice", lambda c, a: [])
        self.assertIsNone(self.connect().use_relative_mouse())

    def test_no_relative_device_is_an_error(self):
        self.server.on("query-mice", lambda c, a: [
            {"name": "QEMU HID Tablet", "index": 1, "current": True, "absolute": True}])
        q = self.connect()
        with self.assertRaisesRegex(qmp.QMPError, "no relative"):
            q.use_relative_mouse()

    def test_a_switch_that_does_not_take_is_an_error(self):
        self.server.on("query-mice", lambda c, a: [
            {"name": "QEMU HID Tablet", "index": 1, "current": True, "absolute": True},
            {"name": "QEMU HID Mouse", "index": 4, "current": False, "absolute": False}])
        q = self.connect()
        with self.assertRaisesRegex(qmp.QMPError, "did not select"):
            q.use_relative_mouse()


class CaptureTest(ServerCase):
    def shots(self):
        d = os.path.join(self.work, "shots")
        return sorted(os.listdir(d)) if os.path.isdir(d) else []

    def serve_screen(self, data, age=0):
        def screendump(_c, line):
            path = line.split(" ")[1]
            with open(path, "wb") as f:
                f.write(data)
            if age:
                old = time.time() - age
                os.utime(path, (old, old))
            return ""
        self.server.on_hmp("screendump", screendump)

    def test_snapshot_returns_the_image_and_cleans_up(self):
        self.serve_screen(ppm(8, 6, (1, 2, 3)))
        q = self.connect()
        w, h, rgb = q.snapshot()
        self.assertEqual((w, h), (8, 6))
        self.assertEqual(rgb[:3], bytes((1, 2, 3)))
        self.assertEqual(self.shots(), [])

    def test_each_capture_asks_for_a_unique_name(self):
        self.serve_screen(ppm())
        q = self.connect()
        q.snapshot()
        q.snapshot()
        a, b = self.server.hmp_log
        self.assertNotEqual(a, b)

    def test_failed_capture_is_an_error_and_a_stale_file_is_not_returned(self):
        q = self.connect(capture_timeout=0.2, name_factory=lambda: "shot-fixed.ppm")
        stale = os.path.join(self.work, "shots", "shot-fixed.ppm")
        os.makedirs(os.path.dirname(stale))
        with open(stale, "wb") as f:
            f.write(ppm(8, 6, (9, 9, 9)))
        self.server.on_hmp("screendump", lambda c, line: "Error: cannot open file")
        with self.assertRaisesRegex(qmp.CaptureError, "no file produced"):
            q.snapshot()
        self.assertFalse(os.path.exists(stale))

    def test_a_capture_older_than_the_request_is_stale(self):
        self.serve_screen(ppm(), age=600)
        q = self.connect()
        with self.assertRaisesRegex(qmp.CaptureError, "stale"):
            q.snapshot()
        self.assertEqual(self.shots(), [])

    def test_truncated_capture_fails_cleanly(self):
        self.serve_screen(ppm(8, 6)[:-10])
        q = self.connect(capture_timeout=0.2)
        with self.assertRaisesRegex(qmp.CaptureError, "truncated"):
            q.snapshot()
        self.assertEqual(self.shots(), [])

    def test_malformed_capture_fails_cleanly(self):
        self.serve_screen(b"not an image at all")
        q = self.connect(capture_timeout=0.2)
        with self.assertRaisesRegex(qmp.CaptureError, "P6"):
            q.snapshot()
        self.assertEqual(self.shots(), [])

    def test_failed_screendump_leaves_no_destination(self):
        self.serve_screen(ppm()[:20])
        q = self.connect(capture_timeout=0.2)
        out = os.path.join(self.work, "out", "screen.png")
        with self.assertRaises(qmp.CaptureError):
            q.screendump(out)
        self.assertFalse(os.path.exists(out))
        self.assertFalse(os.path.exists(os.path.dirname(out)) and os.listdir(os.path.dirname(out)))

    def test_a_capture_directory_the_monitor_would_split_is_refused(self):
        for bad in ("/tmp/My Shots", "/tmp/it's"):
            with self.assertRaisesRegex(qmp.QMPError, "spaces or quotes"):
                qmp.Capture(bad)
        qmp.Capture("/tmp/with space", relative=True)  # a bare name is requested instead

    def test_screendump_writes_png_or_ppm_by_extension(self):
        self.serve_screen(ppm())
        q = self.connect()
        png = q.screendump(os.path.join(self.work, "a.png"))
        ppm_out = q.screendump(os.path.join(self.work, "a.ppm"))
        with open(png, "rb") as f:
            self.assertEqual(f.read(8), b"\x89PNG\r\n\x1a\n")
        self.assertEqual(screen.read_ppm(ppm_out)[:2], (8, 6))
        self.assertEqual([n for n in os.listdir(self.work) if n.startswith(".")], [])

    def test_sandboxed_emulator_is_asked_for_a_bare_name_and_read_from_its_directory(self):
        def screendump(_c, line):
            self.assertEqual(len(line.split()), 2, line)  # no path, no -f flag
            with open(os.path.join(self.server.cwd, line.split()[1]), "wb") as f:
                f.write(ppm())
            return ""
        self.server.on_hmp("screendump", screendump)
        q = self.connect(capture=qmp.Capture(self.server.cwd, relative=True))
        self.assertEqual(q.snapshot()[:2], (8, 6))
        self.assertEqual(os.listdir(self.server.cwd), [])


class IncarnationTest(unittest.TestCase):
    def test_a_recreated_socket_is_a_new_incarnation(self):
        d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, d, True)
        path = os.path.join(d, "s")
        open(path, "w").close()
        first = qmp.socket_incarnation(path)
        self.assertEqual(first, qmp.socket_incarnation(path))
        os.remove(path)
        open(path, "w").close()
        self.assertNotEqual(first, qmp.socket_incarnation(path))

    def test_absent_socket_has_a_fixed_marker(self):
        self.assertEqual(qmp.socket_incarnation("/nonexistent/q.sock"), "absent")


if __name__ == "__main__":
    unittest.main()
