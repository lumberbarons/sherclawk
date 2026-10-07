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


class QMP(qmpdrive.QMP):
    def __init__(self, path=SOCK):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.buf = b""
        self._readline()  # greeting
        self.cmd("qmp_capabilities")
        self._shot_no = 0

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
