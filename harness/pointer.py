"""Pointer tracking for the relative-mouse OS 9 guests.

Routes, all driven by screenshots because the guest pointer's gain
varies with speed and QEMU never reports its position:

* Pointer: template-free closed loop. Park the pointer in the corner, keep
  that screen as a baseline and find the pointer as the arrow-sized blob that
  differs from it. Works over dialogs, menus and text.
* Cursor: a dark-outline template of the arrow, calibrated once.
* blind_step: gain-compensated moves from a remembered position, for menus.

Remembered positions belong to one VM incarnation (GuestState); a restarted or
reset guest starts with none.
"""
import json
import os

import qmp
import screen

STATE_VERSION = 1
REACH = 8           # pixels from the target that still counts as arriving
MAX_MOVE = 120      # mickeys per closed-loop attempt
NEAR_RADIUS = 160   # search this far around the expected pointer before the whole screen


class GuestState:
    """Pointer knowledge for one guest, persisted between helper invocations."""

    def __init__(self, path, incarnation):
        self.path = path
        self.incarnation = incarnation

    def _load(self):
        """The persisted record, or {} for anything missing, unreadable or
        not shaped as this code wrote it."""
        try:
            with open(self.path) as f:
                d = json.load(f)
        except (OSError, ValueError):
            return {}
        if not isinstance(d, dict) or d.get("version") != STATE_VERSION:
            return {}
        pos = d.get("pos")
        if pos is not None and not (isinstance(pos, list) and len(pos) == 2
                                    and all(type(v) is int for v in pos)):
            d["pos"] = None
        offsets = d.get("template")
        if offsets is not None and not (
                isinstance(offsets, list) and offsets
                and all(isinstance(o, list) and len(o) == 2 and all(type(v) is int for v in o)
                        for o in offsets)):
            d["template"] = None
        return d

    def _save(self, d):
        d["version"] = STATE_VERSION
        qmp.atomic_write(self.path, json.dumps(d).encode())

    def pos(self):
        """The remembered pointer position, or None if it is unknown or was
        recorded against another incarnation of the guest."""
        d = self._load()
        pos = d.get("pos")
        if d.get("incarnation") != self.incarnation or pos is None:
            return None
        return (pos[0], pos[1])

    def set_pos(self, pos):
        d = self._load()
        d["incarnation"] = self.incarnation
        d["pos"] = [int(pos[0]), int(pos[1])]
        self._save(d)

    def invalidate(self):
        """Forget the position (the guest was reset); the arrow template stays."""
        d = self._load()
        d["pos"] = None
        self._save(d)

    def template(self):
        offsets = self._load().get("template")
        return [(o[0], o[1]) for o in offsets] if offsets else None

    def set_template(self, offsets):
        d = self._load()
        d["template"] = [list(o) for o in offsets]
        self._save(d)


def _blob_tip(diff, min_pixels, w_range, h_range):
    """Tip of the largest arrow-sized group of differing pixels, or None.
    Groups are 8-connected with a 2px reach because outline pixels only touch."""
    best = None
    seen = set()
    for start in diff:
        if start in seen:
            continue
        blob, stack = [], [start]
        seen.add(start)
        while stack:
            px, py = stack.pop()
            blob.append((px, py))
            for dx in range(-2, 3):
                for dy in range(-2, 3):
                    n = (px + dx, py + dy)
                    if n in diff and n not in seen:
                        seen.add(n)
                        stack.append(n)
        xs = [p[0] for p in blob]
        ys = [p[1] for p in blob]
        bw, bh = max(xs) - min(xs) + 1, max(ys) - min(ys) + 1
        if (len(blob) >= min_pixels and w_range[0] <= bw <= w_range[1]
                and h_range[0] <= bh <= h_range[1]):
            if best is None or len(blob) > len(best):
                best = blob
    if best is None:
        return None
    return (min(p[0] for p in best), min(p[1] for p in best))


def _window(expected, radius, w, h):
    ex, ey = expected
    return (max(0, ex - radius), max(0, ey - radius), min(w, ex + radius + 28), min(h, ey + radius + 28))


class Pointer:
    CORNER = 24          # the parked pointer lives in this square of the baseline
    MIN_PIXELS = 30      # a pointer outline is ~50 differing pixels
    W_RANGE = (6, 20)    # bounding-box limits of the pointer blob
    H_RANGE = (10, 28)

    def __init__(self, q):
        self.q = q
        self.base = None

    def rebase(self):
        for _ in range(12):
            self.q.move(-300, -300)
            self.q.sleep(0.05)
        self.q.sleep(0.6)
        self.base = self.q.snapshot()

    def _diff(self, cur, region):
        w, h, px = cur
        bw, bh, base = self.base
        if (w, h) != (bw, bh):
            raise qmp.QMPError("screen size changed since rebase")
        x0, y0, x1, y1 = region
        stride = w * 3
        diff = set()
        for y in range(y0, y1):
            lo, hi = y * stride + x0 * 3, y * stride + x1 * 3
            if px[lo:hi] == base[lo:hi]:
                continue
            for x in range(x0, x1):
                if x < self.CORNER and y < self.CORNER:
                    continue
                i = y * stride + x * 3
                if (abs(px[i] - base[i]) + abs(px[i + 1] - base[i + 1])
                        + abs(px[i + 2] - base[i + 2])) > 120:
                    diff.add((x, y))
        return diff

    def find(self, expected=None):
        """The pointer tip as (x, y); (0, 0) while it is still parked in the
        corner; None when it cannot be found. With an expected position the
        neighbourhood is searched first and the whole screen only if that fails."""
        cur = self.q.snapshot()
        w, h, _px = cur
        if expected is not None:
            diff = self._diff(cur, _window(expected, NEAR_RADIUS, w, h))
            tip = _blob_tip(diff, self.MIN_PIXELS, self.W_RANGE, self.H_RANGE)
            if tip is not None:
                return tip
        diff = self._diff(cur, (0, 0, w, h))
        if len(diff) < self.MIN_PIXELS:
            return (0, 0)
        return _blob_tip(diff, self.MIN_PIXELS, self.W_RANGE, self.H_RANGE)

    def goto(self, x, y, tolerance=2, attempts=10):
        """Move the pointer tip to (x, y); returns where it ended up."""
        self.rebase()
        pos = self.find()
        gain = 1.0
        for _ in range(attempts):
            if pos is None:
                raise qmp.QMPError("pointer not found; screen changed too much to track")
            dx, dy = x - pos[0], y - pos[1]
            if abs(dx) <= tolerance and abs(dy) <= tolerance:
                return pos
            # Bounded per attempt: with the gain still unknown, a long move
            # can overshoot and clamp at the screen edge, clipping the arrow.
            mx = int(max(-MAX_MOVE, min(MAX_MOVE, round(dx / gain))))
            my = int(max(-MAX_MOVE, min(MAX_MOVE, round(dy / gain))))
            steps = max(1, max(abs(mx), abs(my)) // 20)
            for _i in range(steps):  # equal small steps keep the gain steady
                self.q.move(mx // steps, my // steps)
                self.q.sleep(0.03)
            self.q.sleep(0.4)
            new = self.find(expected=(pos[0] + int(round(mx * gain)), pos[1] + int(round(my * gain))))
            if new is not None and new != pos:
                moved = max(abs(new[0] - pos[0]), abs(new[1] - pos[1]))
                asked = max(abs(mx), abs(my))
                if asked >= 8 and moved:
                    gain = moved / asked
            pos = new
        if pos is None:
            raise qmp.QMPError("pointer not found; screen changed too much to track")
        if abs(x - pos[0]) > REACH or abs(y - pos[1]) > REACH:
            raise qmp.QMPError("pointer stopped at %s, not near (%d, %d)" % (pos, x, y))
        return pos

    def press(self):
        """Wiggle (the guest ignores a press with no preceding motion) and hold."""
        self.q.move(1, 0)
        self.q.sleep(0.1)
        self.q.move(-1, 0)
        self.q.sleep(0.15)
        self.q.button(True)

    def release(self):
        self.q.button(False)
        self.q.sleep(0.3)

    def click(self, hold=0.25):
        self.press()
        try:
            self.q.sleep(hold)
        finally:
            self.release()


class Cursor:
    """Template-tracked pointer: calibrate the arrow's outline once, then
    locate it in screenshots."""

    def __init__(self, q, state, log=lambda *_: None):
        self.q = q
        self.state = state
        self.log = log
        self.prev = None   # (w, h, rgb) last screenshot
        self.pos = None    # (x, y) hotspot estimate
        self.template = state.template()

    def _snap(self):
        return self.q.snapshot()

    def slam_topleft(self):
        for _ in range(6):
            self.q.move(-700, -700)
            self.q.sleep(0.04)
        self.q.sleep(0.35)
        self.prev = self._snap()
        self.pos = (0, 0)
        return self.prev

    def diff_step(self, tx, ty, cap=95):
        """Move towards (tx, ty) using screen diffs as the only position source
        (calibration, when no template exists yet)."""
        dx = tx - self.pos[0]
        dy = ty - self.pos[1]
        mx = int(max(-cap, min(cap, dx)))
        my = int(max(-cap, min(cap, dy)))
        if mx == 0 and my == 0:
            return self.pos
        w, h, rgb0 = self.prev
        self.q.move(mx, my)
        self.q.sleep(0.3)
        w, h, rgb1 = self._snap()
        expected = (self.pos[0] + int(mx * 1.5), self.pos[1] + int(my * 1.5))
        cands = []
        for b in screen.diff_boxes(w, h, rgb0, rgb1):
            if not (3 <= b[2] - b[0] <= 44 and 3 <= b[3] - b[1] <= 44):
                continue
            if abs(b[0] - self.pos[0]) <= 3 and abs(b[1] - self.pos[1]) <= 3:
                continue  # the vacated position
            cands.append(b)
        if cands:
            b = min(cands, key=lambda b: (b[0] - expected[0]) ** 2 + (b[1] - expected[1]) ** 2)
            self.pos = (b[0], b[1])
        self.prev = (w, h, rgb1)
        return self.pos

    def recover(self):
        """Slam into the corner then nudge right-down so the arrow is fully
        on-screen; re-anchor the position from the screen diff."""
        self.slam_topleft()
        w0, h0, rgb0 = self.prev
        self.q.move(45, 35)
        self.q.sleep(0.35)
        w, h, rgb1 = self._snap()
        boxes = [b for b in screen.diff_boxes(w, h, rgb0, rgb1)
                 if 3 <= b[2] - b[0] <= 44 and 3 <= b[3] - b[1] <= 44]
        if boxes:
            b = max(boxes, key=lambda b: b[0] + b[1])
            self.pos = (b[0], b[1])
        else:
            self.pos = (0, 0)
        self.prev = (w, h, rgb1)
        return self.pos

    def verify_template(self):
        """Recover to a fully visible cursor and confirm the template locates it."""
        if not self.template:
            return False
        for _attempt in range(2):
            self.recover()
            w, h, rgb = self.prev
            loc = self.locate(w, h, rgb, expected=self.pos)
            if loc:
                self.log("  template verify: score=%.3f at (%d,%d) pos=%s" % (loc[2], loc[0], loc[1], self.pos))
            if (loc and loc[2] >= 0.9 and abs(loc[0] - self.pos[0]) <= 30
                    and abs(loc[1] - self.pos[1]) <= 30):
                self.pos = (loc[0], loc[1])
                return True
        return False

    def calibrate(self, target=None):
        """Walk the pointer to a clean desktop area (diff-tracked) and take a
        template of the arrow's dark outline there."""
        w, h, _rgb = self.slam_topleft()
        target = target or (w // 3, h // 2)
        for _ in range(20):
            if abs(self.pos[0] - target[0]) <= 60 and abs(self.pos[1] - target[1]) <= 60:
                break
            self.diff_step(*target)
        else:
            raise qmp.QMPError("calibrate: could not reach clean area; pos=%s" % (self.pos,))
        w, h, cur = self.prev
        x0, y0 = self.pos
        pad = 3
        offs = []
        for y in range(max(0, y0 - pad), min(h, y0 + 24 + pad)):
            for x in range(max(0, x0 - pad), min(w, x0 + 24 + pad)):
                i = (y * w + x) * 3
                if cur[i] < 110 and cur[i + 1] < 110 and cur[i + 2] < 110:
                    offs.append((x - x0, y - y0))
        if not (40 <= len(offs) <= 140):
            raise qmp.QMPError("calibrate: template has %d offsets, suspicious (pos=%s)" % (len(offs), self.pos))
        self.template = offs
        self.state.set_template(offs)
        loc = self.locate(w, h, cur, expected=(x0, y0))
        self.log("  calibrate: self-check at %s" % (loc,))
        if not loc or loc[2] < 0.9 or abs(loc[0] - x0) > 3 or abs(loc[1] - y0) > 3:
            raise qmp.QMPError("calibrate: self-check failed")
        return (x0, y0)

    def locate(self, w, h, rgb, expected=None):
        """Best template match, searching near the expected position first and
        the whole screen only when nothing matches there."""
        if not self.template:
            return None
        if expected is not None:
            res = screen.find_template(
                w, h, screen.dark_mask(w, h, rgb, region=_window(expected, NEAR_RADIUS, w, h)),
                self.template)
            if res:
                return res[0]
        res = screen.find_template(w, h, screen.dark_mask(w, h, rgb), self.template)
        if not res:
            return None
        if expected is None:
            return res[0]
        ex, ey = expected
        top = res[0][2]
        near = [r for r in res if r[2] >= top - 0.05]
        return min(near, key=lambda r: abs(r[0] - ex) + abs(r[1] - ey))

    def step_towards(self, tx, ty, max_mickey=110):
        dx = tx - self.pos[0]
        dy = ty - self.pos[1]
        mx = int(max(-max_mickey, min(max_mickey, dx)))
        my = int(max(-max_mickey, min(max_mickey, dy)))
        if mx == 0 and my == 0:
            return self.pos
        w, h, rgb0 = self.prev
        self.q.move(mx, my)
        self.q.sleep(0.28)
        w, h, rgb1 = self._snap()
        expected = (self.pos[0] + int(mx * 1.6), self.pos[1] + int(my * 1.6))
        loc = self.locate(w, h, rgb1, expected=expected)
        if loc and loc[2] >= 0.8 and abs(loc[0] - expected[0]) + abs(loc[1] - expected[1]) <= 260:
            self.pos = (loc[0], loc[1])
            self.prev = (w, h, rgb1)
            return self.pos
        # fall back to the displacement measured from screen diffs
        boxes = [b for b in screen.diff_boxes(w, h, rgb0, rgb1)
                 if 3 <= b[2] - b[0] <= 48 and 3 <= b[3] - b[1] <= 48]
        if boxes:
            b = min(boxes, key=lambda b: (b[0] - expected[0]) ** 2 + (b[1] - expected[1]) ** 2)
            self.pos = (b[0], b[1])
        self.prev = (w, h, rgb1)
        return self.pos

    def goto(self, tx, ty, tol=4, max_steps=30):
        stalls = 0
        last = None
        recoveries = 0
        for _ in range(max_steps):
            x, y = self.pos
            if abs(tx - x) <= tol and abs(ty - y) <= tol:
                return self.pos
            if last == self.pos:
                stalls += 1
                if stalls >= 2:
                    if recoveries >= 3:
                        break
                    recoveries += 1
                    self.recover()  # edge clamp / lost tracking
                    stalls = 0
                    last = self.pos
                    continue
            else:
                stalls = 0
            last = self.pos
            self.step_towards(tx, ty)
        return self.pos

    def click_at(self, x, y, button="left", tol=4, retries=1):
        for _attempt in range(retries + 1):
            self.goto(x, y, tol=tol)
            d = abs(self.pos[0] - x) + abs(self.pos[1] - y)
            if d <= 12:
                self.q.sleep(0.12)
                self.q.click(button)
                self.q.sleep(0.12)
                return self.pos
            self.log("  click_at: off target by %dpx (pos=%s, target=(%d,%d)), retrying" % (d, self.pos, x, y))
        raise qmp.QMPError("click_at: could not reach (%d,%d); stuck at %s" % (x, y, self.pos))

    def doubleclick_at(self, x, y, button="left", tol=4):
        self.click_at(x, y, button=button, tol=tol, retries=1)
        self.q.sleep(0.06)
        self.q.click(button)
        self.q.sleep(0.12)


def blind_step(q, state, pos, tx, ty, gain=1.6, chunk=110):
    """Move toward (tx, ty) with gain-compensated relative moves and no visual
    feedback, remembering the believed position. Safe where template matching
    is unreliable (menus, dialogs); check a screenshot before clicking."""
    while True:
        dx, dy = tx - pos[0], ty - pos[1]
        if abs(dx) <= 2 and abs(dy) <= 2:
            return pos
        mx = int(max(-chunk, min(chunk, round(dx / gain))))
        my = int(max(-chunk, min(chunk, round(dy / gain))))
        if mx == 0 and my == 0:
            return pos
        q.move(mx, my)
        q.sleep(0.3)
        pos = (pos[0] + int(round(mx * gain)), pos[1] + int(round(my * gain)))
        state.set_pos(pos)
        if abs(mx) == chunk or abs(my) == chunk:
            continue
        return pos


def default_state_dir():
    return os.path.join(os.environ.get("TMPDIR", "/tmp"), "os9-guest-state")
