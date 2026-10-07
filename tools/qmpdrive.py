#!/usr/bin/env python3
"""
qmpdrive.py - Minimal QMP/HMP client + screen analysis helpers for driving
the Mac OS 9 QEMU VM during automated installation.

Stdlib only. Talks to a QMP unix socket (default /tmp/macqmp.sock).

Cursor tracking strategy:
  * slam the pointer into the top-left corner (clamps at 0,0)
  * calibrate: walk it to a clean desktop area using screen diffs
    (ground truth pixel movement), then extract a CONTAMINATION-FREE
    arrow template (dark outline pixels only, over plain background)
  * afterwards locate() finds the arrow via masked template matching,
    with candidates filtered by score and expected proximity; falls back
    to diff-displacement when the arrow cannot be matched (e.g. cursor
    shape changed to I-beam / watch).
"""

import json
import os
import socket
import struct
import sys
import time
import zlib

QMP_SOCK = os.environ.get("QMP_SOCK", "/tmp/macqmp.sock")
TEMPLATE_FILE = os.environ.get("CURSOR_TEMPLATE", "/tmp/mac_cursor_template.json")


class QMP:
    def __init__(self, path=QMP_SOCK):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.buf = b""
        self._readline()  # greeting
        self.cmd("qmp_capabilities")

    def _readline(self):
        while b"\n" not in self.buf:
            chunk = self.sock.recv(1 << 20)
            if not chunk:
                raise EOFError("QMP connection closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        return json.loads(line) if line.strip() else None

    def cmd(self, name, **args):
        self.sock.sendall(
            (json.dumps({"execute": name, "arguments": args}) + "\n").encode()
        )
        while True:
            obj = self._readline()
            if obj is None:
                continue
            if "event" in obj:
                continue
            if "error" in obj:
                raise RuntimeError(f"QMP error for {name}: {obj['error']}")
            return obj.get("return")

    def hmp(self, command):
        return self.cmd("human-monitor-command", **{"command-line": command})

    # ---- high level helpers -------------------------------------------------

    def screendump(self, path, fmt="ppm"):
        self.hmp(f"screendump {path} -f {fmt}")
        return path

    def move(self, dx, dy):
        evs = []
        if dx:
            evs.append({"type": "rel", "data": {"axis": "x", "value": int(dx)}})
        if dy:
            evs.append({"type": "rel", "data": {"axis": "y", "value": int(dy)}})
        if evs:
            self.cmd("input-send-event", events=evs)

    def button(self, down, button="left"):
        self.cmd(
            "input-send-event",
            events=[{"type": "btn", "data": {"down": down, "button": button}}],
        )

    def click(self, button="left", settle=0.06):
        self.button(True, button)
        time.sleep(settle)
        self.button(False, button)

    def doubleclick(self, button="left", gap=0.09):
        self.click(button)
        time.sleep(gap)
        self.click(button)

    def key(self, *qcodes):
        self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes])

    def type_text(self, text):
        KEYMAP = {
            " ": "spc", "-": "minus", ".": "dot", ",": "comma", "/": "slash",
            "0": "0", "1": "1", "2": "2", "3": "3", "4": "4",
            "5": "5", "6": "6", "7": "7", "8": "8", "9": "9",
        }
        SHIFTED = {"_": "minus", "~": "grave_accent", "!": "1", "?": "slash"}
        for ch in text:
            if ch.isalpha():
                if ch.islower():
                    self.key(ch.lower())
                else:
                    self.key("shift", ch.lower())
            elif ch in KEYMAP:
                self.key(KEYMAP[ch])
            elif ch in SHIFTED:
                self.key("shift", SHIFTED[ch])
            else:
                raise ValueError(f"no keymap for {ch!r}")
            time.sleep(0.04)


# ---- framebuffer analysis --------------------------------------------------

def read_ppm(path):
    """Read a P6 PPM, return (width, height, pixels) with pixels as bytes RGB."""
    with open(path, "rb") as f:
        data = f.read()
    pos = 2
    fields = []
    while len(fields) < 3:
        while pos < len(data) and data[pos : pos + 1].isspace():
            pos += 1
        if data[pos : pos + 1] == b"#":
            while data[pos : pos + 1] != b"\n":
                pos += 1
            continue
        start = pos
        while pos < len(data) and not data[pos : pos + 1].isspace():
            pos += 1
        fields.append(int(data[start:pos]))
    pos += 1
    w, h, maxv = fields
    assert maxv == 255, maxv
    return w, h, data[pos : pos + w * h * 3]


def write_png(path, w, h, rgb):
    def chunk(tag, payload):
        c = struct.pack(">I", len(payload)) + tag + payload
        return c + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)
        raw += rgb[y * stride : (y + 1) * stride]
    png = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(bytes(raw), 6))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as f:
        f.write(png)


def ppm_to_png(src, dst):
    w, h, rgb = read_ppm(src)
    write_png(dst, w, h, rgb)
    return dst


def crop_zoom(ppm, x0, y0, w0, h0, scale, out):
    """Crop a region of a ppm and save as a scaled-up PNG (for inspection)."""
    w, h, rgb = read_ppm(ppm)
    x0, y0 = max(0, x0), max(0, y0)
    cw, ch = min(w0, w - x0), min(h0, h - y0)
    scaled = bytearray()
    for y in range(ch):
        row = bytearray()
        for x in range(cw):
            i = ((y0 + y) * w + (x0 + x)) * 3
            row += rgb[i : i + 3] * scale
        scaled += row * scale
    write_png(out, cw * scale, ch * scale, bytes(scaled))
    return out


def dark_mask(w, h, rgb, lum_thresh=110):
    pts = set()
    for y in range(h):
        row = y * w * 3
        for x in range(w):
            i = row + x * 3
            if rgb[i] < lum_thresh and rgb[i + 1] < lum_thresh and rgb[i + 2] < lum_thresh:
                pts.add((x, y))
    return pts


def diff_boxes(w, h, rgb_a, rgb_b, thresh=48):
    """Connected components of changed pixels; returns bounding boxes."""
    changed = set()
    for i in range(0, w * h * 3, 3):
        if (
            abs(rgb_a[i] - rgb_b[i]) > thresh
            or abs(rgb_a[i + 1] - rgb_b[i + 1]) > thresh
            or abs(rgb_a[i + 2] - rgb_b[i + 2]) > thresh
        ):
            p = i // 3
            changed.add((p % w, p // w))
    boxes = []
    while changed:
        seed = changed.pop()
        stack = [seed]
        minx = maxx = seed[0]
        miny = maxy = seed[1]
        while stack:
            cx, cy = stack.pop()
            for nx in (cx - 1, cx, cx + 1):
                for ny in (cy - 1, cy, cy + 1):
                    if (nx, ny) in changed:
                        changed.discard((nx, ny))
                        stack.append((nx, ny))
                        if nx < minx: minx = nx
                        if nx > maxx: maxx = nx
                        if ny < miny: miny = ny
                        if ny > maxy: maxy = ny
        boxes.append((minx, miny, maxx, maxy))
    return boxes


def bright_mask(w, h, rgb, lum_thresh=200):
    pts = set()
    for y in range(h):
        row = y * w * 3
        for x in range(w):
            i = row + x * 3
            if rgb[i] > lum_thresh and rgb[i + 1] > lum_thresh and rgb[i + 2] > lum_thresh:
                pts.add((x, y))
    return pts


def find_template(w, h, dark_pts, tmpl, bright_pts=None, bright_tmpl=None,
                  min_dark=0.8, max_results=400):
    """Masked template match.
    tmpl: dark offsets that must be dark; bright_tmpl: offsets that should be
    bright (cursor's own white body). Ranks by min(dark_frac, bright_frac);
    background junk in bright_tmpl merely lowers all candidates together.
    Returns [(x, y, combined_score)] ranked, deterministic order."""
    from collections import defaultdict
    n = len(tmpl)
    scores = defaultdict(int)
    probes = [tmpl[0], tmpl[n // 2], tmpl[-1], tmpl[n // 3]]
    for (dx, dy) in probes:
        for (px, py) in dark_pts:
            scores[(px - dx, py - dy)] += 1
    results = []
    nb = len(bright_tmpl) if bright_tmpl else 0
    for (x, y), seed in scores.items():
        if seed < 2 or x < 0 or y < 0 or x >= w or y >= h:
            continue
        matched = sum(1 for (dx, dy) in tmpl if (x + dx, y + dy) in dark_pts)
        score = matched / n
        if score < min_dark:
            continue
        if bright_tmpl:
            # soft tiebreak only: the bright set is sampled once during
            # calibration and may be stale on very different backgrounds
            bm = sum(1 for (dx, dy) in bright_tmpl if (x + dx, y + dy) in bright_pts)
            score += 0.001 * (bm / nb)
        results.append((x, y, score))
    results.sort(key=lambda r: (-r[2], r[1], r[0]))
    return results[:max_results]


class Cursor:
    def __init__(self, qmp, workdir="/tmp", template_path=TEMPLATE_FILE):
        self.q = qmp
        self.wd = workdir
        self.seq = 0
        self.prev = None   # (w, h, rgb) last screenshot
        self.pos = None    # (x, y) hotspot estimate
        self.template = None        # dark offsets (arrow outline)
        self.bright_template = None # bright offsets (arrow white body)
        self.template_path = template_path

    # -- screenshots -----------------------------------------------------

    def _snap(self):
        self.seq += 1
        path = os.path.join(self.wd, f"snap{self.seq}.ppm")
        self.q.screendump(path)
        w, h, rgb = read_ppm(path)
        return w, h, rgb

    def slam_topleft(self):
        for _ in range(6):
            self.q.move(-700, -700)
            time.sleep(0.04)
        time.sleep(0.35)
        w, h, rgb = self._snap()
        self.prev = (w, h, rgb)
        self.pos = (0, 0)
        return w, h, rgb

    def diff_step(self, tx, ty, cap=95):
        """Move towards (tx,ty) using screen diffs as the only position source
        (used during calibration when no template exists yet)."""
        assert self.pos is not None
        dx = tx - self.pos[0]
        dy = ty - self.pos[1]
        mx = int(max(-cap, min(cap, dx)))
        my = int(max(-cap, min(cap, dy)))
        if mx == 0 and my == 0:
            return self.pos
        w, h, rgb0 = self.prev
        self.q.move(mx, my)
        time.sleep(0.3)
        w, h, rgb1 = self._snap()
        expected = (self.pos[0] + int(mx * 1.5), self.pos[1] + int(my * 1.5))
        cands = []
        for b in diff_boxes(w, h, rgb0, rgb1):
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

    # -- template management ---------------------------------------------

    def save_template(self):
        json.dump(
            {"offsets": self.template, "bright": self.bright_template or []},
            open(self.template_path, "w"),
        )

    def load_template(self):
        try:
            d = json.load(open(self.template_path))
            self.template = [tuple(o) for o in d["offsets"]]
            self.bright_template = [tuple(o) for o in d.get("bright", [])]
            return bool(self.template)
        except Exception:
            return False

    def recover(self):
        """Slam into the corner then nudge right-down so the arrow is fully
        on-screen; re-anchor position from the screen diff."""
        self.slam_topleft()
        w0, h0, rgb0 = self.prev
        self.q.move(45, 35)
        time.sleep(0.35)
        w, h, rgb1 = self._snap()
        boxes = [
            b for b in diff_boxes(w, h, rgb0, rgb1)
            if 3 <= b[2] - b[0] <= 44 and 3 <= b[3] - b[1] <= 44
        ]
        if boxes:
            b = max(boxes, key=lambda b: b[0] + b[1])
            self.pos = (b[0], b[1])
        else:
            self.pos = (0, 0)
        self.prev = (w, h, rgb1)
        return self.pos

    def verify_template(self):
        """Recover to a fully visible cursor and confirm the template locates
        it; also confirms the bright-offset set does not over-reject."""
        if not self.template:
            return False
        for attempt in range(2):
            self.recover()
            w, h, rgb = self.prev
            loc = self.locate(w, h, rgb, expected=self.pos)
            if loc:
                print(f"  template verify: score={loc[2]:.3f} at ({loc[0]},{loc[1]}) pos={self.pos}")
            if (
                loc
                and loc[2] >= 0.9
                and abs(loc[0] - self.pos[0]) <= 30
                and abs(loc[1] - self.pos[1]) <= 30
            ):
                self.pos = (loc[0], loc[1])
                return True
        return False

    def calibrate(self, target=(480, 260)):
        """Walk the cursor to a clean desktop area (diff-tracked) and extract
        a contamination-free template of the arrow's dark outline."""
        self.slam_topleft()
        for i in range(14):
            if abs(self.pos[0] - target[0]) <= 60 and abs(self.pos[1] - target[1]) <= 60:
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
        if not (40 <= len(offs) <= 120):
            raise RuntimeError(f"calibrate: template has {len(offs)} offsets, suspicious (pos={self.pos})")
        self.template = offs
        self.pos = pos = (x0, y0)
        # Phase 2: white-body offsets, sampled with the cursor over the dark
        # 'untitled' disk icon (bright pixels there == cursor's own whites).
        # These are used to *rank* matches (min(dark, bright)); background
        # junk in the set merely lowers all candidates equally.
        for attempt in range(10):
            if abs(self.pos[0] - 582) <= 8 and abs(self.pos[1] - 116) <= 8:
                break
            self.diff_step(582, 116, cap=60)
        w2, h2, cur2 = self._snap()
        x2, y2 = self.pos
        win_dark = win_bright = 0
        for y in range(y2, min(h2, y2 + 22)):
            for x in range(x2, min(w2, x2 + 22)):
                i = (y * w2 + x) * 3
                m = min(cur2[i], cur2[i + 1], cur2[i + 2])
                if m < 110:
                    win_dark += 1
                elif m > 200:
                    win_bright += 1
        print(f"  calibrate phase2: pos={self.pos} dark={win_dark} bright={win_bright}")
        if win_dark < 120:
            raise RuntimeError(f"calibrate: phase2 not over a dark area (pos={self.pos})")
        bright = []
        for y in range(y2, min(h2, y2 + 24)):
            for x in range(x2, min(w2, x2 + 24)):
                i = (y * w2 + x) * 3
                if cur2[i] > 200 and cur2[i + 1] > 200 and cur2[i + 2] > 200:
                    bright.append((x - x2, y - y2))
        keep = []
        for (dx, dy) in bright:
            px, py = x0 + dx, y0 + dy
            i = (py * w + px) * 3
            if cur[i] > 200 and cur[i + 1] > 200 and cur[i + 2] > 200:
                keep.append((dx, dy))
        print(f"  calibrate: {len(offs)} dark + {len(keep)} bright offsets")
        if not (8 <= len(keep) <= 140):
            raise RuntimeError(f"calibrate: bright offsets suspicious: {len(keep)}")
        self.bright_template = keep
        self.save_template()
        # validate against the clean capture
        loc = self.locate(w, h, cur, expected=(x0, y0))
        print(f"  calibrate: self-check at {loc}")
        if not loc or loc[2] < 0.9 or abs(loc[0] - x0) > 2 or abs(loc[1] - y0) > 2:
            raise RuntimeError("calibrate: self-check failed")
        # current state: cursor is over the dark icon
        self.pos = (x2, y2)
        self.prev = (w2, h2, cur2)
        return (x2, y2)

    # -- locating ---------------------------------------------------------

    def locate(self, w, h, rgb, expected=None):
        if not self.template:
            return None
        dp = dark_mask(w, h, rgb)
        bp = bright_mask(w, h, rgb) if self.bright_template else None
        res = find_template(
            w, h, dp, self.template,
            bright_pts=bp, bright_tmpl=self.bright_template,
            min_dark=0.8,
        )
        if not res:
            return None
        if expected is None:
            return res[0]
        ex, ey = expected
        def dist(r):
            return abs(r[0] - ex) + abs(r[1] - ey)
        # 1) strong candidates near the expected position win, unless the
        #    global best is both much better and plausibly close anyway
        near_exp = [r for r in res if dist(r) <= 120]
        if near_exp:
            top_near = max(near_exp, key=lambda r: r[2])
            top_any = res[0]
            if top_near[2] >= top_any[2] - 0.15 or dist(top_any) > 260:
                return top_near
        # 2) otherwise: among candidates nearly as good as the best, the
        #    closest to the expected position
        top = res[0][2]
        near = [r for r in res if r[2] >= top - 0.05]
        return min(near, key=dist)

    # -- movement ----------------------------------------------------------

    def step_towards(self, tx, ty, max_mickey=110):
        assert self.pos is not None, "slam/load template first"
        dx = tx - self.pos[0]
        dy = ty - self.pos[1]
        # keep steps moderate: feedback handles guest-side acceleration
        mx = int(max(-max_mickey, min(max_mickey, dx)))
        my = int(max(-max_mickey, min(max_mickey, dy)))
        if mx == 0 and my == 0:
            return self.pos
        w, h, rgb0 = self.prev
        self.q.move(mx, my)
        time.sleep(0.28)
        w, h, rgb1 = self._snap()
        expected = (self.pos[0] + int(mx * 1.6), self.pos[1] + int(my * 1.6))
        loc = self.locate(w, h, rgb1, expected=expected)
        if loc and loc[2] >= 0.8:
            # reject absurd jumps (>260px from where we told it to go)
            if abs(loc[0] - expected[0]) + abs(loc[1] - expected[1]) <= 260:
                self.pos = (loc[0], loc[1])
                self.prev = (w, h, rgb1)
                return self.pos
        # fallback: measure actual displacement from screen diffs
        boxes = [
            b for b in diff_boxes(w, h, rgb0, rgb1)
            if 3 <= b[2] - b[0] <= 48 and 3 <= b[3] - b[1] <= 48
        ]
        if boxes:
            def bscore(b):
                return (b[0] - expected[0]) ** 2 + (b[1] - expected[1]) ** 2
            b = min(boxes, key=bscore)
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
                    self.recover()  # edge clamp / lost tracking recovery
                    stalls = 0
                    last = self.pos
                    continue
            else:
                stalls = 0
            last = self.pos
            self.step_towards(tx, ty)
        return self.pos

    def click_at(self, x, y, button="left", tol=4, retries=1):
        for attempt in range(retries + 1):
            self.goto(x, y, tol=tol)
            d = abs(self.pos[0] - x) + abs(self.pos[1] - y)
            if d <= 12:
                time.sleep(0.12)
                self.q.click(button)
                time.sleep(0.12)
                return self.pos
            print(f"  click_at: off target by {d}px (pos={self.pos}, target=({x},{y})), retrying")
        raise RuntimeError(f"click_at: could not reach ({x},{y}); stuck at {self.pos}")

    def doubleclick_at(self, x, y, button="left", tol=4):
        self.click_at(x, y, button=button, tol=tol, retries=1)
        # first click done by click_at; do the second click now
        time.sleep(0.06)
        self.q.click(button)
        time.sleep(0.12)


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "help"
    q = QMP()
    if cmd == "screendump":
        q.screendump(sys.argv[2], fmt="png")
    elif cmd == "hmp":
        print(q.hmp(sys.argv[2]))
    elif cmd == "ppm2png":
        ppm_to_png(sys.argv[2], sys.argv[3])
    elif cmd == "calibrate":
        c = Cursor(q)
        pos = c.calibrate()
        print("calibrated at", pos)
    elif cmd == "verify":
        c = Cursor(q)
        c.load_template()
        print("verify:", c.verify_template())
    elif cmd == "prepare":
        c = Cursor(q)
        if c.load_template() and c.verify_template():
            print("template OK, cursor at", c.pos)
        else:
            print("template missing/stale; calibrating...")
            pos = c.calibrate()
            print("calibrated at", pos)
    elif cmd == "help":
        print("commands: screendump <png> | hmp <cmd> | ppm2png <in> <out> | calibrate | verify")
