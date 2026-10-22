"""Host tests for pointer tracking against a synthetic guest screen."""
import json
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import pointer  # noqa: E402
import qmp  # noqa: E402
import screen  # noqa: E402

W, H = 320, 240
ARROW = [(x, y) for y in range(17) for x in range(int(y * 0.6) + 2)]


class FakeGuest:
    """A light desktop with a dark arrow at the pointer; relative moves are
    scaled by a gain and clamp at the screen edges, as the real guest's do."""

    def __init__(self, gain=1.5, start=(100, 80)):
        self.gain = gain
        self.pos = start
        self.buttons = []
        self.snapshots = 0

    def sleep(self, _seconds):
        pass

    def move(self, dx, dy):
        x = min(W - 1, max(0, self.pos[0] + int(dx * self.gain)))
        y = min(H - 1, max(0, self.pos[1] + int(dy * self.gain)))
        self.pos = (x, y)

    def button(self, down, button="left"):
        self.buttons.append(down)

    def click(self, button="left"):
        self.buttons.extend([True, False])

    def snapshot(self):
        self.snapshots += 1
        rgb = bytearray(b"\xdc" * (W * H * 3))
        for dx, dy in ARROW:
            x, y = self.pos[0] + dx, self.pos[1] + dy
            if x < W and y < H:
                i = (y * W + x) * 3
                rgb[i:i + 3] = b"\x10\x10\x10"
        return W, H, bytes(rgb)


class PointerTest(unittest.TestCase):
    def test_goto_reaches_the_target_whatever_the_gain(self):
        for gain in (1.0, 1.3, 1.6):
            g = FakeGuest(gain=gain)
            pos = pointer.Pointer(g).goto(200, 120)
            self.assertLessEqual(abs(pos[0] - 200), 2, gain)
            self.assertLessEqual(abs(pos[1] - 120), 2, gain)
            self.assertEqual(g.pos, pos)

    def test_goto_fails_instead_of_returning_a_position_it_did_not_reach(self):
        g = FakeGuest()
        g.move = lambda dx, dy: None  # the guest ignores the pointer
        with self.assertRaisesRegex(qmp.QMPError, "not near"):
            pointer.Pointer(g).goto(200, 120)

    def test_goto_fails_when_the_pointer_is_lost(self):
        g = FakeGuest()
        p = pointer.Pointer(g)
        real = p.find
        calls = []

        def find(expected=None):
            calls.append(expected)
            return real(expected) if len(calls) == 1 else None
        p.find = find
        with self.assertRaisesRegex(qmp.QMPError, "not found"):
            p.goto(200, 120)

    def test_the_near_search_is_centred_on_where_the_move_should_have_landed(self):
        g = FakeGuest(gain=1.0)
        p = pointer.Pointer(g)
        seen = []
        real = p.find

        def find(expected=None):
            seen.append(expected)
            return real(expected)
        p.find = find
        p.goto(200, 120)
        self.assertIsNone(seen[0])
        self.assertNotEqual(seen[1], (200, 120))  # the first leg stops short of the target
        self.assertLessEqual(abs(seen[1][0] - 120), 2)

    def test_find_reports_the_corner_while_parked(self):
        g = FakeGuest(start=(0, 0))
        p = pointer.Pointer(g)
        p.rebase()
        self.assertEqual(p.find(), (0, 0))

    def test_find_searches_near_the_expected_position_first(self):
        g = FakeGuest()
        p = pointer.Pointer(g)
        p.rebase()
        g.pos = (210, 130)
        regions = []
        real = p._diff

        def spy(cur, region):
            regions.append(region)
            return real(cur, region)
        p._diff = spy
        self.assertEqual(p.find(expected=(200, 120)), (210, 130))
        self.assertEqual(len(regions), 1)
        self.assertLess(regions[0][2] - regions[0][0], W)

    def test_find_falls_back_to_the_whole_screen(self):
        g = FakeGuest()
        p = pointer.Pointer(g)
        p.rebase()
        g.pos = (280, 200)
        regions = []
        real = p._diff

        def spy(cur, region):
            regions.append(region)
            return real(cur, region)
        p._diff = spy
        self.assertEqual(p.find(expected=(20, 20)), (280, 200))
        self.assertEqual(regions[-1], (0, 0, W, H))
        self.assertEqual(len(regions), 2)

    def test_find_returns_none_when_nothing_arrow_sized_changed(self):
        g = FakeGuest()
        p = pointer.Pointer(g)
        p.rebase()
        g.pos = (0, 0)
        g.snapshot = lambda: (W, H, b"\x00" * (W * H * 3))  # everything changed: one huge blob
        self.assertIsNone(p.find())

    def test_screen_size_change_is_an_error(self):
        g = FakeGuest()
        p = pointer.Pointer(g)
        p.rebase()
        g.snapshot = lambda: (8, 6, b"\x00" * (8 * 6 * 3))
        with self.assertRaisesRegex(qmp.QMPError, "size changed"):
            p.find()

    def test_click_releases_even_if_the_hold_is_interrupted(self):
        g = FakeGuest()
        calls = []

        def sleep(seconds):
            calls.append(seconds)
            if seconds == 0.25:
                raise KeyboardInterrupt
        g.sleep = sleep
        with self.assertRaises(KeyboardInterrupt):
            pointer.Pointer(g).click()
        self.assertEqual(g.buttons, [True, False])


class CursorTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.state = pointer.GuestState(os.path.join(self.dir, "state.json"), "inc-1")

    def test_calibrate_learns_the_arrow_and_persists_it(self):
        g = FakeGuest(start=(0, 0))
        c = pointer.Cursor(g, self.state)
        c.calibrate()
        self.assertTrue(40 <= len(c.template) <= 140)
        self.assertEqual(sorted(self.state.template()), sorted(c.template))

    def test_goto_follows_the_template_to_the_target(self):
        g = FakeGuest(start=(0, 0))
        c = pointer.Cursor(g, self.state)
        c.calibrate()
        c.recover()
        pos = c.goto(230, 150)
        self.assertLessEqual(abs(pos[0] - 230), 4)
        self.assertLessEqual(abs(pos[1] - 150), 4)
        self.assertLessEqual(abs(g.pos[0] - pos[0]) + abs(g.pos[1] - pos[1]), 6)

    def test_locate_searches_near_the_expected_position_before_the_whole_screen(self):
        g = FakeGuest(start=(150, 100))
        c = pointer.Cursor(g, self.state)
        c.template = [(x, y) for x, y in ARROW if x == 0 or y == 16]
        w, h, rgb = g.snapshot()
        masks = []
        real = screen.dark_mask

        def spy(w_, h_, rgb_, lum_thresh=110, region=None):
            masks.append(region)
            return real(w_, h_, rgb_, lum_thresh, region)
        with mock.patch.object(screen, "dark_mask", spy):
            loc = c.locate(w, h, rgb, expected=(140, 90))
        self.assertEqual(loc[:2], (150, 100))
        self.assertEqual(len(masks), 1)
        self.assertIsNotNone(masks[0])

    def test_locate_falls_back_to_the_whole_screen(self):
        g = FakeGuest(start=(280, 200))
        c = pointer.Cursor(g, self.state)
        c.template = [(x, y) for x, y in ARROW if x == 0 or y == 16]
        w, h, rgb = g.snapshot()
        loc = c.locate(w, h, rgb, expected=(10, 10))
        self.assertLessEqual(abs(loc[0] - 280) + abs(loc[1] - 200), 2)

    def test_calibrate_refuses_a_screen_with_no_arrow(self):
        g = FakeGuest(start=(0, 0))
        g.snapshot = lambda: (W, H, b"\xdc" * (W * H * 3))
        with self.assertRaises(qmp.QMPError):
            pointer.Cursor(g, self.state).calibrate()


class GuestStateTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.path = os.path.join(self.dir, "s", "state.json")

    def test_position_round_trips_for_the_same_incarnation(self):
        a = pointer.GuestState(self.path, "inc-1")
        self.assertIsNone(a.pos())
        a.set_pos((12, 34))
        self.assertEqual(pointer.GuestState(self.path, "inc-1").pos(), (12, 34))

    def test_a_new_incarnation_does_not_inherit_the_old_position(self):
        pointer.GuestState(self.path, "inc-1").set_pos((12, 34))
        self.assertIsNone(pointer.GuestState(self.path, "inc-2").pos())

    def test_invalidate_forgets_the_position_but_keeps_the_template(self):
        s = pointer.GuestState(self.path, "inc-1")
        s.set_template([(0, 0), (1, 2)])
        s.set_pos((5, 6))
        s.invalidate()
        self.assertIsNone(s.pos())
        self.assertEqual(s.template(), [(0, 0), (1, 2)])

    def test_unreadable_or_foreign_state_is_treated_as_empty(self):
        os.makedirs(os.path.dirname(self.path))
        for content in ("not json", json.dumps({"version": 0, "pos": [1, 2], "incarnation": "i"}),
                        json.dumps({"version": 1, "incarnation": "i", "pos": "x"})):
            with open(self.path, "w") as f:
                f.write(content)
            s = pointer.GuestState(self.path, "i")
            self.assertIsNone(s.pos())
            self.assertIsNone(s.template())

    def test_malformed_values_in_the_state_file_are_treated_as_missing(self):
        os.makedirs(os.path.dirname(self.path))
        for rec in ({"version": 1, "incarnation": "i", "pos": ["a", 1]},
                    {"version": 1, "incarnation": "i", "pos": [1.5, 2]},
                    {"version": 1, "incarnation": "i", "template": [1, 2]},
                    {"version": 1, "incarnation": "i", "template": [["x", 1]]}):
            with open(self.path, "w") as f:
                json.dump(rec, f)
            s = pointer.GuestState(self.path, "i")
            self.assertIsNone(s.pos())
            self.assertIsNone(s.template())

    def test_blind_step_moves_and_remembers_the_believed_position(self):
        g = FakeGuest(gain=1.6, start=(0, 0))
        s = pointer.GuestState(self.path, "inc-1")
        pos = pointer.blind_step(g, s, (0, 0), 100, 60)
        self.assertLessEqual(abs(pos[0] - 100), 3)
        self.assertEqual(s.pos(), pos)


if __name__ == "__main__":
    unittest.main()
