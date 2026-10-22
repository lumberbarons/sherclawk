"""Bounded QMP client for the OS 9 guests: commands, input and screenshots.

Stdlib only. Every operation has a deadline, so a wedged guest or a socket
held by another client becomes an error instead of a hang. Commands carry ids
and only the matching response is accepted; asynchronous events are recorded,
and a late reply to an abandoned command is discarded rather than mistaken for
the answer to the next one.
"""
import collections
import contextlib
import hashlib
import json
import os
import socket
import time
import uuid

import screen

DEFAULT_TIMEOUT = 20.0
KEY_HOLD_MS = 200   # the guest drops presses held for less (QMP defaults to 100)
KEY_GAP = 0.1       # release time after the hold, so no key merges with the next
CAPTURE_TIMEOUT = 5.0
STALE_SLACK = 2.0   # seconds of filesystem timestamp granularity to forgive


class QMPError(Exception):
    pass


class QMPTimeout(QMPError):
    pass


class QMPDisconnected(QMPError):
    pass


class QMPCommandError(QMPError):
    pass


class KeymapError(QMPError, ValueError):
    pass


class CaptureError(QMPError):
    pass


KEYS = {
    " ": "spc", "-": "minus", ".": "dot", ",": "comma", "/": "slash",
    ";": "semicolon", "'": "apostrophe", "=": "equal", "[": "bracket_left",
    "]": "bracket_right", "\\": "backslash", "`": "grave_accent",
    "\n": "ret", "\t": "tab",
}
SHIFTED = {
    "_": "minus", "~": "grave_accent", "!": "1", "@": "2", "#": "3", "$": "4",
    "%": "5", "^": "6", "&": "7", "*": "8", "(": "9", ")": "0", "+": "equal",
    ":": "semicolon", '"': "apostrophe", "?": "slash", "<": "comma",
    ">": "dot", "{": "bracket_left", "}": "bracket_right", "|": "backslash",
}


def plan_text(text):
    """Map text to key chords, refusing the whole string if any character
    has no key, so a partial string is never typed into the guest."""
    chords = []
    for i, ch in enumerate(text):
        if ch.isascii() and ch.isalnum():
            chords.append(("shift", ch.lower()) if ch.isupper() else (ch,))
        elif ch in KEYS:
            chords.append((KEYS[ch],))
        elif ch in SHIFTED:
            chords.append(("shift", SHIFTED[ch]))
        else:
            raise KeymapError("no key for %r at offset %d; nothing typed" % (ch, i))
    return chords


def socket_incarnation(path):
    """Identify one run of the VM behind a QMP socket.

    QEMU creates the socket when it starts, so a new file means a new
    incarnation; state recorded against an old one must not be reused.
    """
    try:
        st = os.stat(path)
    except OSError:
        return "absent"
    raw = "%d:%d:%d" % (st.st_dev, st.st_ino, st.st_ctime_ns)
    return hashlib.sha1(raw.encode()).hexdigest()[:16]


def atomic_write(path, data):
    path = os.path.abspath(path)
    directory = os.path.dirname(path)
    os.makedirs(directory, exist_ok=True)
    tmp = os.path.join(directory, ".%s.%s.tmp" % (os.path.basename(path), uuid.uuid4().hex[:8]))
    try:
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
    finally:
        with contextlib.suppress(FileNotFoundError):
            os.remove(tmp)
    return path


class Capture:
    """Where the emulator drops screenshots and how it is asked to.

    A standalone QEMU writes to any absolute path (-f ppm). UTM's sandboxed
    QEMU only accepts a bare filename and writes it into its own working
    directory, so the file is requested by name and read from there.
    """

    def __init__(self, directory, relative=False):
        if not relative and any(c.isspace() or c in "\"'" for c in directory):
            # The monitor splits its command line on whitespace.
            raise QMPError("capture directory %r must not contain spaces or quotes" % directory)
        self.directory = directory
        self.relative = relative

    def command(self, name):
        if self.relative:
            return "screendump " + name
        return "screendump %s -f ppm" % os.path.join(self.directory, name)

    def path(self, name):
        return os.path.join(self.directory, name)


def unique_name():
    return "shot-%d-%s.ppm" % (os.getpid(), uuid.uuid4().hex[:12])


class QMP:
    def __init__(self, path, timeout=DEFAULT_TIMEOUT, capture=None, sleep=time.sleep,
                 key_hold_ms=KEY_HOLD_MS, key_gap=KEY_GAP, capture_timeout=None,
                 name_factory=unique_name):
        self.path = path
        self.timeout = timeout
        self.sleep = sleep
        self.key_hold_ms = key_hold_ms
        self.key_gap = key_gap
        self.capture = capture or Capture(os.path.join(os.environ.get("TMPDIR", "/tmp"),
                                                       "os9-guest-shots"))
        self.capture_timeout = CAPTURE_TIMEOUT if capture_timeout is None else capture_timeout
        self.name_factory = name_factory
        self.events = collections.deque(maxlen=256)
        self.resets = 0
        self.closed = False
        self.buf = b""
        self._next_id = 0
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            self.sock.settimeout(timeout)
            try:
                self.sock.connect(path)
            except OSError as e:
                raise QMPError("cannot connect to %s: %s" % (path, e)) from None
            try:
                greeting = self._read(time.monotonic() + timeout)
            except QMPTimeout:
                raise QMPTimeout("QMP greeting timed out: another process may hold %s" % path) from None
            if "QMP" not in greeting:
                raise QMPError("not a QMP greeting: %r" % (greeting,))
            self.cmd("qmp_capabilities")
        except BaseException:
            self.close()
            raise

    # ---- protocol --------------------------------------------------------

    def _read(self, deadline):
        while b"\n" not in self.buf:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise QMPTimeout("QMP read timed out")
            self.sock.settimeout(remaining)
            try:
                chunk = self.sock.recv(1 << 20)
            except socket.timeout:
                raise QMPTimeout("QMP read timed out") from None
            except OSError as e:
                self.close()
                raise QMPDisconnected("QMP connection lost: %s" % e) from None
            if not chunk:
                self.close()
                raise QMPDisconnected("QMP connection closed")
            self.buf += chunk
        line, self.buf = self.buf.split(b"\n", 1)
        if not line.strip():
            return self._read(deadline)
        try:
            obj = json.loads(line)
        except ValueError:
            raise QMPError("malformed QMP message: %r" % line[:80]) from None
        if not isinstance(obj, dict):
            raise QMPError("unexpected QMP message: %r" % (obj,))
        return obj

    def _note_event(self, obj):
        self.events.append(obj)
        if obj.get("event") == "RESET":
            self.resets += 1

    def cmd(self, name, timeout=None, **args):
        if self.closed:
            raise QMPDisconnected("QMP connection is closed")
        self._next_id += 1
        cid = self._next_id
        msg = {"execute": name, "id": cid}
        if args:
            msg["arguments"] = args
        deadline = time.monotonic() + (self.timeout if timeout is None else timeout)
        try:
            self.sock.settimeout(self.timeout)
            self.sock.sendall((json.dumps(msg) + "\n").encode())
        except socket.timeout:
            raise QMPTimeout("QMP %s: send timed out" % name) from None
        except OSError as e:
            self.close()
            raise QMPDisconnected("QMP connection lost: %s" % e) from None
        while True:
            try:
                obj = self._read(deadline)
            except QMPTimeout:
                raise QMPTimeout("QMP %s timed out" % name) from None
            if "event" in obj:
                self._note_event(obj)
            elif obj.get("id") == cid:
                if "error" in obj:
                    raise QMPCommandError("QMP error for %s: %s" % (name, obj["error"]))
                return obj.get("return")
            elif "id" not in obj and "error" in obj:
                raise QMPCommandError("QMP error for %s: %s" % (name, obj["error"]))
            # otherwise: a late reply to a command that already timed out

    def hmp(self, command, timeout=None):
        return self.cmd("human-monitor-command", timeout=timeout, **{"command-line": command})

    def close(self):
        self.closed = True
        with contextlib.suppress(OSError):
            self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # ---- input -------------------------------------------------------------

    def key(self, *qcodes):
        """Press the keys together for key_hold_ms, then wait out the release
        gap, so consecutive presses never overlap."""
        if not qcodes:
            raise ValueError("key needs at least one qcode")
        self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes],
                 **{"hold-time": self.key_hold_ms})
        self.sleep(self.key_hold_ms / 1000.0 + self.key_gap)

    def type_text(self, text):
        for chord in plan_text(text):
            self.key(*chord)

    def move(self, dx, dy):
        events = []
        if dx:
            events.append({"type": "rel", "data": {"axis": "x", "value": int(dx)}})
        if dy:
            events.append({"type": "rel", "data": {"axis": "y", "value": int(dy)}})
        if events:
            self.cmd("input-send-event", events=events)

    def button(self, down, button="left"):
        self.cmd("input-send-event",
                 events=[{"type": "btn", "data": {"down": bool(down), "button": button}}])

    def click(self, button="left", hold=0.06):
        """Press and release; the button is released even if the hold fails."""
        try:
            self.button(True, button)
            self.sleep(hold)
        except BaseException:
            with contextlib.suppress(QMPError):
                self.button(False, button)
            raise
        self.button(False, button)

    def doubleclick(self, button="left", gap=0.09):
        self.click(button)
        self.sleep(gap)
        self.click(button)

    def mice(self):
        return self.cmd("query-mice")

    def use_relative_mouse(self):
        """Make a relative pointing device current and return it (None when
        the guest lists no pointing devices at all).

        Which device works varies by machine (UTM's guest needs a HID mouse
        that is not the one selected at boot), so the device number is found
        from query-mice and never hardcoded. An absolute tablet ignores the
        relative moves the harness sends.
        """
        mice = self.mice()
        if not mice:
            return None  # nothing to choose between; input events still route
        relative = [m for m in mice if not m.get("absolute")]
        if not relative:
            raise QMPError("guest has no relative pointing device: %r" % (mice,))
        for m in relative:
            if m.get("current"):
                return m
        choice = next((m for m in relative if "hid" in m.get("name", "").lower()), relative[0])
        self.hmp("mouse_set %d" % choice["index"])
        if not any(m.get("current") and m["index"] == choice["index"] for m in self.mice()):
            raise QMPError("mouse_set %d did not select %r" % (choice["index"], choice.get("name")))
        return choice

    def system_reset(self):
        self.cmd("system_reset")

    # ---- screenshots -------------------------------------------------------

    def snapshot(self):
        """Capture the screen; return (width, height, rgb).

        The file name is unique to this call, so an earlier capture can never
        stand in for a failed one; a file older than the request, or one that
        stays malformed or truncated, is an error, and the temporary capture
        is always removed.
        """
        name = self.name_factory()
        path = self.capture.path(name)
        if not self.capture.relative:
            os.makedirs(self.capture.directory, mode=0o700, exist_ok=True)
        with contextlib.suppress(FileNotFoundError):
            os.remove(path)
        started = time.time()
        try:
            reply = self.hmp(self.capture.command(name))
            deadline = time.monotonic() + self.capture_timeout
            reason = "no file produced (monitor said %r)" % (reply,)
            while True:
                try:
                    if os.stat(path).st_mtime < started - STALE_SLACK:
                        raise CaptureError("stale capture %s: older than the request" % name)
                    return screen.read_ppm(path)
                except FileNotFoundError:
                    pass
                except screen.ImageError as e:
                    reason = str(e)
                if time.monotonic() >= deadline:
                    raise CaptureError("screendump failed: %s" % reason)
                time.sleep(0.02)
        finally:
            with contextlib.suppress(FileNotFoundError):
                os.remove(path)

    def screendump(self, path):
        """Save a screenshot as PNG or PPM by extension, written atomically."""
        w, h, rgb = self.snapshot()
        if path.lower().endswith(".png"):
            return atomic_write(path, screen.encode_png(w, h, rgb))
        return atomic_write(path, screen.encode_ppm(w, h, rgb))
