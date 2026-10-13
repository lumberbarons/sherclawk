#!/usr/bin/env python3
"""
utm_qmp.py — drive the UTM "Mac OS 9.2.2" VM through its extra QMP socket.

The VM's config.plist carries an additional QEMU argument:

    -qmp unix:<UTM container>/tmp/utm-mac9-qmp.sock,server=on,wait=off

so QEMU exposes a QMP monitor just for host-side automation.  This tool
reuses qmpdrive.py (originally written to drive the command-line QEMU VM
during the OS 9 install) to take screenshots, track the guest cursor and
click on things. Keyboard commands bypass cursor calibration so they work
even when a frozen guest or stale pointer template prevents mouse tracking.

Usage:
    utm_qmp.py screendump <out.png>
    utm_qmp.py hmp '<monitor command>'
    utm_qmp.py prepare                 # load or (re)calibrate cursor template
    utm_qmp.py reanchor                # slam cursor to a known spot (resync)
    utm_qmp.py locate                  # where is the guest cursor right now?
    utm_qmp.py goto <x> <y>            # template-tracked move
    utm_qmp.py moveto <x> <y>          # blind gain-compensated move (menus)
    utm_qmp.py click <x> <y>
    utm_qmp.py clicknow                # click where the cursor already is
    utm_qmp.py dclicknow               # double-click where the cursor already is
    utm_qmp.py dclick <x> <y>
    utm_qmp.py type "text"
    utm_qmp.py key <qcode> [<qcode>...]

Closed-loop pointer commands (no template; they work over dialogs, menus
and text, where goto/click lose the pointer). Each re-parks the pointer in the
corner and re-measures, so the pointer ends within a couple of pixels:
    utm_qmp.py pgoto <x> <y>           # move the pointer tip to x, y
    utm_qmp.py pclick <x> <y>          # move there and click
    utm_qmp.py pdclick <x> <y>         # move there and double-click
    utm_qmp.py phold <x> <y>           # move there and hold the button down
    utm_qmp.py nudge <dx> <dy>         # raw relative move (menus, popups)
    utm_qmp.py release                 # let the button up

The QMP socket takes one client at a time; do not run two of these at once
(a script holding a QMP connection must use its own screendump).
"""
import os
import shutil
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qmpdrive  # noqa: E402

SOCK = os.environ.get(
    "UTM_QMP_SOCK",
    os.path.expanduser(
        "~/Library/Group Containers/WDNLXAD4W8.com.utmapp.UTM/utm-mac9-qmp.sock"
    ),
)
# UTM's QEMU process runs sandboxed with its working directory set to the
# UTM group container; that is the only place it may create files (and it
# is where the SPICE and QMP sockets live).
GROUP = os.path.expanduser("~/Library/Group Containers/WDNLXAD4W8.com.utmapp.UTM")
SNAP_DIR = os.path.join(os.environ.get("TMPDIR", "/tmp"), "utm-snaps")
os.makedirs(SNAP_DIR, exist_ok=True)
STATE = os.path.join(SNAP_DIR, "cursor-pos.json")


def save_pos(pos):
    with open(STATE, "w") as f:
        json = __import__("json")
        json.dump(list(pos), f)


def load_pos():
    try:
        with open(STATE) as f:
            import json
            return tuple(json.load(f))
    except Exception:
        return None


KEY_HOLD_MS = 200  # the guest drops keys held for less (QMP's default is 100)
KEY_GAP = 0.3      # release time between keys so none is merged with the next
TYPE_KEYS = {
    " ": "spc", "-": "minus", ".": "dot", ",": "comma", "/": "slash",
    ";": "semicolon", "'": "apostrophe", "=": "equal", "\n": "ret", "\t": "tab",
}
TYPE_SHIFTED = {
    "_": "minus", "~": "grave_accent", "!": "1", "?": "slash", ":": "semicolon",
    "+": "equal", "(": "9", ")": "0", "@": "2", "#": "3", "*": "8", '"': "apostrophe",
}


class QMP(qmpdrive.QMP):
    """The QMP socket serves one client at a time: a second process connecting
    while this one is open (a helper shelling out to `utm_qmp.py screendump`,
    say) never gets its greeting. The timeout turns that hang into an error."""

    def __init__(self, path=SOCK):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(20)
        try:
            self.sock.connect(path)
            self.buf = b""
            self._readline()  # greeting
        except socket.timeout:
            raise RuntimeError(
                "QMP greeting timed out: another process may hold the socket"
            ) from None
        self.cmd("qmp_capabilities")
        self._shot_no = 0

    def key(self, *qcodes):
        """Press the keys together for KEY_HOLD_MS, then wait KEY_GAP."""
        self.cmd(
            "send-key",
            keys=[{"type": "qcode", "data": k} for k in qcodes],
            **{"hold-time": KEY_HOLD_MS},
        )
        time.sleep(KEY_GAP)

    def type_text(self, text):
        for ch in text:
            if ch.isascii() and ch.isalnum():
                self.key(*(["shift", ch.lower()] if ch.isupper() else [ch]))
            elif ch in TYPE_KEYS:
                self.key(TYPE_KEYS[ch])
            elif ch in TYPE_SHIFTED:
                self.key("shift", TYPE_SHIFTED[ch])
            else:
                raise ValueError(f"no keymap for {ch!r}")

    def screendump(self, path, fmt="ppm"):
        """Shoot a screenshot into the group container, then copy it out.

        HMP screendump here takes only a relative filename (no format flag)
        and writes a PPM next to QEMU's cwd inside the sandbox.
        """
        self._shot_no += 1
        name = f"utm-shot-{self._shot_no}.ppm"
        res = self.hmp(f"screendump {name}")
        src = os.path.join(GROUP, name)
        if not os.path.exists(src):
            raise RuntimeError(f"screendump failed: {res!r}")
        dst = os.path.abspath(path)
        os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
        shutil.copyfile(src, dst)
        return dst


class Cursor(qmpdrive.Cursor):
    """Cursor tracking for the UTM VM (1280x720 guest display).

    Only difference from the original: calibration doesn't sample the
    cursor's white body against a specific icon position (that trick was
    tied to the 640x480 install screen); the dark-outline template alone
    locates the arrow fine, the bright set is only a ranking tiebreak.
    """

    def calibrate(self, target=(400, 300)):
        self.slam_topleft()
        for _ in range(20):
            if (
                abs(self.pos[0] - target[0]) <= 60
                and abs(self.pos[1] - target[1]) <= 60
            ):
                break
            self.diff_step(*target)
        else:
            raise RuntimeError(f"calibrate: could not reach clean area; pos={self.pos}")

        w, h, cur = self.prev
        x0, y0 = self.pos
        pad = 3
        xa, ya = max(0, x0 - pad), max(0, y0 - pad)
        xb, yb = min(w, x0 + 24 + pad), min(h, y0 + 24 + pad)
        offs = []
        for y in range(ya, yb):
            for x in range(xa, xb):
                i = (y * w + x) * 3
                if cur[i] < 110 and cur[i + 1] < 110 and cur[i + 2] < 110:
                    offs.append((x - x0, y - y0))
        if not (40 <= len(offs) <= 140):
            raise RuntimeError(
                f"calibrate: template has {len(offs)} offsets, suspicious (pos={self.pos})"
            )
        self.template = offs
        self.bright_template = []
        self.save_template()

        loc = self.locate(w, h, cur, expected=(x0, y0))
        print(f"  calibrate: self-check at {loc}")
        if not loc or loc[2] < 0.9 or abs(loc[0] - x0) > 3 or abs(loc[1] - y0) > 3:
            raise RuntimeError("calibrate: self-check failed")
        return (x0, y0)


class Pointer:
    """Template-free closed-loop pointer control for the relative-mouse guest.

    The guest pointer's gain varies with move speed, and template matching
    loses it over dialogs and text. Instead park the pointer in the top-left
    corner, keep that screen as a baseline, and find the pointer later as the
    arrow-sized blob of pixels that differ from it. Blinking carets, the clock
    and redraws are not arrow-sized, so they are ignored. Rebase whenever the
    screen contents change enough to matter; each move re-measures the pointer
    and corrects, so gain does not need to be known.
    """

    CORNER = 24          # the parked pointer lives in this square of the baseline
    MIN_PIXELS = 30      # a pointer outline is ~50 differing pixels
    W_RANGE = (6, 20)    # bounding-box limits of the pointer blob
    H_RANGE = (10, 28)

    def __init__(self, q):
        self.q = q
        self.base = None

    def _shot(self):
        path = os.path.join(SNAP_DIR, "pointer.ppm")
        self.q.screendump(path)
        w, h, px = qmpdrive.read_ppm(path)
        os.remove(path)
        return w, h, px

    def rebase(self):
        for _ in range(12):
            self.q.move(-300, -300)
            time.sleep(0.05)
        time.sleep(0.6)
        self.base = self._shot()

    def find(self):
        """Return the (x, y) of the pointer tip, or None when it is not found."""
        w, h, cur = self._shot()
        bw, bh, base = self.base
        if (w, h) != (bw, bh):
            raise RuntimeError("screen size changed since rebase")
        stride = w * 3
        diff = set()
        for y in range(h):
            if cur[y * stride:(y + 1) * stride] == base[y * stride:(y + 1) * stride]:
                continue
            for x in range(w):
                if x < self.CORNER and y < self.CORNER:
                    continue
                i = y * stride + x * 3
                if (abs(cur[i] - base[i]) + abs(cur[i + 1] - base[i + 1])
                        + abs(cur[i + 2] - base[i + 2])) > 120:
                    diff.add((x, y))
        if len(diff) < self.MIN_PIXELS:
            return (0, 0)  # still parked in the corner
        best = None
        seen = set()
        for start in diff:
            if start in seen:
                continue
            blob, stack = [], [start]
            seen.add(start)
            while stack:  # 8-connected with a 2px reach: outline pixels touch
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
            bw_, bh_ = max(xs) - min(xs) + 1, max(ys) - min(ys) + 1
            if (len(blob) >= self.MIN_PIXELS
                    and self.W_RANGE[0] <= bw_ <= self.W_RANGE[1]
                    and self.H_RANGE[0] <= bh_ <= self.H_RANGE[1]):
                if best is None or len(blob) > len(best):
                    best = blob
        if best is None:
            return None
        top = min(p[1] for p in best)
        return (min(p[0] for p in best), top)

    def goto(self, x, y, tolerance=2, attempts=10):
        """Move the pointer tip to (x, y); returns where it ended up."""
        self.rebase()
        pos = self.find()
        gain = 1.0
        for _ in range(attempts):
            if pos is None:
                raise RuntimeError("pointer not found; screen changed too much to track")
            dx, dy = x - pos[0], y - pos[1]
            if abs(dx) <= tolerance and abs(dy) <= tolerance:
                return pos
            mx = int(round(dx / gain))
            my = int(round(dy / gain))
            steps = max(1, max(abs(mx), abs(my)) // 20)
            for _i in range(steps):  # equal small steps keep the gain steady
                self.q.move(mx // steps, my // steps)
                time.sleep(0.03)
            time.sleep(0.4)
            new = self.find()
            if new is not None and new != pos:
                moved = max(abs(new[0] - pos[0]), abs(new[1] - pos[1]))
                asked = max(abs(mx), abs(my))
                if asked >= 8 and moved:
                    gain = moved / asked
            pos = new
        return pos

    def press(self):
        """Wiggle (the guest ignores a press with no preceding motion) and hold."""
        self.q.move(1, 0)
        time.sleep(0.1)
        self.q.move(-1, 0)
        time.sleep(0.15)
        self.q.button(True)

    def release(self):
        self.q.button(False)
        time.sleep(0.3)

    def click(self, hold=0.25):
        self.press()
        time.sleep(hold)
        self.release()


def prepare(q):
    c = Cursor(q, workdir=SNAP_DIR)
    if c.load_template() and c.verify_template():
        print(f"template OK, cursor at {c.pos}")
        return c
    print("template missing/stale; calibrating...")
    c.calibrate()
    print(f"calibrated, cursor at {c.pos}")
    return c


def blind_step(c, q, tx, ty, gain=1.6, chunk=110):
    """Move toward (tx, ty) with gain-compensated relative moves and no
    visual feedback. Safe in text-heavy areas (menus, dialogs) where
    template matching is unreliable; verify the result with a screenshot
    before clicking."""
    while True:
        dx, dy = tx - c.pos[0], ty - c.pos[1]
        if abs(dx) <= 2 and abs(dy) <= 2:
            return c.pos
        mx = int(max(-chunk, min(chunk, round(dx / gain))))
        my = int(max(-chunk, min(chunk, round(dy / gain))))
        if mx == 0 and my == 0:
            return c.pos
        q.move(mx, my)
        time.sleep(0.3)
        c.pos = (c.pos[0] + int(round(mx * gain)), c.pos[1] + int(round(my * gain)))
        save_pos(c.pos)
        if abs(mx) == chunk or abs(my) == chunk:
            continue
        return c.pos


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "help"
    q = QMP()
    if cmd == "screendump":
        out = sys.argv[2]
        q.screendump(out)
        if out.lower().endswith(".png"):
            qmpdrive.ppm_to_png(out, out)
    elif cmd == "hmp":
        print(q.hmp(sys.argv[2]))
    elif cmd == "key":
        q.key(*sys.argv[2:])
    elif cmd == "type":
        q.type_text(sys.argv[2])
    elif cmd in ("pgoto", "pclick", "pdclick", "phold"):
        ptr = Pointer(q)
        pos = ptr.goto(int(sys.argv[2]), int(sys.argv[3]))
        print("pointer at", pos)
        if cmd == "pclick":
            ptr.click()
        elif cmd == "pdclick":
            ptr.click()
            ptr.click()
        elif cmd == "phold":
            ptr.press()
    elif cmd == "nudge":
        q.move(int(sys.argv[2]), int(sys.argv[3]))
    elif cmd == "release":
        q.button(False)
    elif cmd == "prepare":
        prepare(q)
    elif cmd == "locate":
        c = Cursor(q, workdir=SNAP_DIR)
        c.load_template()
        c.recover()
        print(c.pos)
    elif cmd == "reanchor":
        c = Cursor(q, workdir=SNAP_DIR)
        if not c.load_template():
            c.calibrate()
        c.recover()
        save_pos(c.pos)
        print("anchored at", c.pos)
    elif cmd in ("moveto", "clicknow", "dclicknow"):
        c = Cursor(q, workdir=SNAP_DIR)
        pos = load_pos()
        if pos is None:
            if not c.load_template():
                c.calibrate()
            c.recover()
            save_pos(c.pos)
        c.pos = load_pos()
        if cmd == "moveto":
            blind_step(c, q, int(sys.argv[2]), int(sys.argv[3]))
            print("believed cursor at", c.pos)
        elif cmd == "clicknow":
            q.click()
            time.sleep(0.2)
            print("clicked at", c.pos)
        else:
            q.click()
            time.sleep(0.08)
            q.click()
            time.sleep(0.1)
            print("double-clicked at", c.pos)
    else:
        c = prepare(q)
        if cmd == "goto":
            print(c.goto(int(sys.argv[2]), int(sys.argv[3])))
        elif cmd == "click":
            print(c.click_at(int(sys.argv[2]), int(sys.argv[3])))
        elif cmd == "dclick":
            print(c.doubleclick_at(int(sys.argv[2]), int(sys.argv[3])))
        else:
            print(__doc__)


if __name__ == "__main__":
    main()
