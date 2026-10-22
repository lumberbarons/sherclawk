"""Host tests for the guest command line against a fake QMP server."""
import contextlib
import io
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import fake_qmp  # noqa: E402
import guest  # noqa: E402
import pointer  # noqa: E402
import qmp  # noqa: E402
from fake_qmp import ppm  # noqa: E402


class GuestCliTest(unittest.TestCase):
    def setUp(self):
        self.server = fake_qmp.FakeQMP()
        self.addCleanup(self.server.close)
        self.work = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.work, True)

    def run_cli(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            rc = guest.main(["--socket", self.server.path, "--state-dir", self.work, *argv])
        return rc, out.getvalue(), err.getvalue()

    def state(self):
        key = guest.hashlib.sha1(os.path.abspath(self.server.path).encode()).hexdigest()[:10]
        return pointer.GuestState(os.path.join(self.work, "guest-%s.json" % key),
                                  qmp.socket_incarnation(self.server.path))

    def test_key_sends_one_press(self):
        rc, _out, _err = self.run_cli("key", "meta_l", "o")
        self.assertEqual(rc, 0)
        (args,) = self.server.commands("send-key")
        self.assertEqual([k["data"] for k in args["keys"]], ["meta_l", "o"])

    def test_unmappable_text_exits_nonzero_having_typed_nothing(self):
        rc, _out, err = self.run_cli("type", "ok☃")
        self.assertEqual(rc, 1)
        self.assertIn("nothing typed", err)
        self.assertEqual(self.server.commands("send-key"), [])

    def test_screendump_writes_a_png(self):
        def screendump(_c, line):
            with open(line.split(" ")[1], "wb") as f:
                f.write(ppm())
            return ""
        self.server.on_hmp("screendump", screendump)
        out = os.path.join(self.work, "s.png")
        rc, _o, _e = self.run_cli("screendump", out)
        self.assertEqual(rc, 0)
        with open(out, "rb") as f:
            self.assertEqual(f.read(4), b"\x89PNG")

    def test_a_failed_screendump_exits_nonzero_without_output(self):
        self.server.on_hmp("screendump", lambda c, line: "Error")
        out = os.path.join(self.work, "s.png")
        saved, qmp.CAPTURE_TIMEOUT = qmp.CAPTURE_TIMEOUT, 0.2
        self.addCleanup(setattr, qmp, "CAPTURE_TIMEOUT", saved)
        rc, _o, err = self.run_cli("screendump", out)
        self.assertEqual(rc, 1)
        self.assertIn("screendump failed", err)
        self.assertFalse(os.path.exists(out))

    def test_mice_lists_devices(self):
        self.server.on("query-mice", lambda c, a: [
            {"name": "QEMU PS/2 Mouse", "index": 0, "current": True, "absolute": False},
            {"name": "QEMU HID Tablet", "index": 1, "current": False, "absolute": True}])
        rc, out, _e = self.run_cli("mice")
        self.assertEqual(rc, 0)
        self.assertEqual(out.splitlines(), ["* #0: QEMU PS/2 Mouse", "  #1: QEMU HID Tablet (absolute)"])

    def test_pointer_commands_select_a_relative_device_first(self):
        self.server.on("query-mice", lambda c, a: [
            {"name": "QEMU HID Tablet", "index": 1, "current": True, "absolute": True}])
        rc, _o, err = self.run_cli("nudge", "5", "5")
        self.assertEqual(rc, 1)
        self.assertIn("no relative", err)
        self.assertEqual(self.server.commands("input-send-event"), [])

    def test_input_without_click_types_without_touching_the_pointing_device(self):
        self.assertEqual(self.run_cli("input", "ab")[0], 0)
        self.assertEqual(self.server.commands("query-mice"), [])
        self.assertEqual(len(self.server.commands("send-key")), 2)

    def test_input_click_is_not_sent_when_the_text_cannot_be_typed(self):
        rc, _o, err = self.run_cli("input", "--click", "ok\u2603")
        self.assertEqual(rc, 1)
        self.assertIn("nothing typed", err)
        self.assertEqual(self.server.commands("input-send-event"), [])

    def test_locate_does_not_calibrate(self):
        # No template is stored; recovering the position must not need one.
        self.server.on_hmp("screendump", lambda c, line: "Error")
        saved, qmp.CAPTURE_TIMEOUT = qmp.CAPTURE_TIMEOUT, 0.1
        self.addCleanup(setattr, qmp, "CAPTURE_TIMEOUT", saved)
        self.run_cli("locate")
        moves = self.server.commands("input-send-event")
        # slam_topleft only: calibrate would have followed with diff steps
        self.assertTrue(all(e["events"][0]["data"]["value"] == -700 for e in moves))

    def test_a_reset_seen_in_a_failing_command_still_forgets_the_pointer(self):
        self.state().set_pos((10, 20))

        def key_then_fail(conn, a):
            conn.send({"event": "RESET"})
            raise fake_qmp.Fail("boom")
        self.server.on("send-key", key_then_fail)
        rc, _o, _e = self.run_cli("key", "esc")
        self.assertEqual(rc, 1)
        self.assertIsNone(self.state().pos())

    def test_ppm2png_converts_without_a_guest(self):
        src = os.path.join(self.work, "a.ppm")
        with open(src, "wb") as f:
            f.write(ppm())
        dst = os.path.join(self.work, "a.png")
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = guest.main(["ppm2png", src, dst])
        self.assertEqual(rc, 0)
        with open(dst, "rb") as f:
            self.assertEqual(f.read(4), b"\x89PNG")

    def test_keys_do_not_touch_the_pointing_device(self):
        self.run_cli("key", "esc")
        self.assertEqual(self.server.commands("query-mice"), [])

    def test_reset_resets_the_guest_and_forgets_the_pointer(self):
        self.state().set_pos((10, 20))
        rc, _o, _e = self.run_cli("reset")
        self.assertEqual(rc, 0)
        self.assertEqual(len(self.server.commands("system_reset")), 1)
        self.assertIsNone(self.state().pos())

    def test_a_reset_seen_during_a_command_forgets_the_pointer(self):
        self.state().set_pos((10, 20))
        self.server.on("send-key", lambda conn, a: conn.send({"event": "RESET"}) or {})
        rc, _o, _e = self.run_cli("key", "esc")
        self.assertEqual(rc, 0)
        self.assertIsNone(self.state().pos())

    def test_nudge_and_release_send_input(self):
        self.assertEqual(self.run_cli("nudge", "3", "-4")[0], 0)
        self.assertEqual(self.run_cli("release")[0], 0)
        evs = [a["events"] for a in self.server.commands("input-send-event")]
        self.assertEqual(evs[0][0]["data"]["value"], 3)
        self.assertFalse(evs[-1][0]["data"]["down"])

    def test_no_guest_chosen_is_an_error(self):
        os.environ.pop("QMP_SOCK", None)
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            rc = guest.main(["--state-dir", self.work, "type", "x"])
        self.assertEqual(rc, 1)
        self.assertIn("choose a guest", err.getvalue())


class ResolveTest(unittest.TestCase):
    def parse(self, *argv):
        return guest.build_parser().parse_args([*argv, "mice"])

    def test_slot_resolves_to_the_provisioned_socket(self):
        path, capture, _d = guest.resolve(self.parse("--slot", "slot2", "--home", "/h"))
        self.assertEqual(path, "/h/run/slot2.qmp")
        self.assertFalse(capture.relative)

    def test_utm_asks_for_bare_names_in_its_container(self):
        path, capture, _d = guest.resolve(self.parse("--utm"))
        self.assertTrue(capture.relative)
        self.assertTrue(path.endswith("utm-mac9-qmp.sock"))
        self.assertEqual(capture.directory, guest.UTM_GROUP)

    def test_socket_falls_back_to_the_environment(self):
        os.environ["QMP_SOCK"] = "/x/y.sock"
        self.addCleanup(os.environ.pop, "QMP_SOCK", None)
        self.assertEqual(guest.resolve(self.parse())[0], "/x/y.sock")


if __name__ == "__main__":
    unittest.main()
