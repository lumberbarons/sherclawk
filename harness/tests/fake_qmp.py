"""A scriptable fake QMP server on a unix socket, for host tests (no QEMU)."""
import json
import os
import shutil
import socket
import tempfile
import threading
import time


class Drop(Exception):
    """Handler outcome: close the connection without replying."""


class Silent(Exception):
    """Handler outcome: reply to nothing."""


class Fail(Exception):
    """Handler outcome: reply with a QMP error."""


def ppm(w=8, h=6, fill=(200, 200, 200)):
    return b"P6\n%d %d\n255\n" % (w, h) + bytes(fill) * (w * h)


class Conn:
    def __init__(self, sock):
        self.sock = sock

    def send(self, obj):
        self.sock.sendall((json.dumps(obj) + "\n").encode())


class FakeQMP:
    def __init__(self):
        # The path has to stay under the unix socket length limit.
        self.dir = tempfile.mkdtemp(prefix="q", dir="/tmp")
        self.path = os.path.join(self.dir, "q.sock")
        self.cwd = os.path.join(self.dir, "cwd")  # where a sandboxed QEMU writes bare names
        os.makedirs(self.cwd)
        self.greeting = {"QMP": {"version": {"qemu": {"major": 8}}, "capabilities": []}}
        self.log = []          # (monotonic time, command, arguments)
        self.hmp_log = []
        self.handlers = {name: (lambda ctx, args: {})
                         for name in ("qmp_capabilities", "send-key", "input-send-event",
                                      "system_reset")}
        self.hmp_handlers = {}
        self.handlers["human-monitor-command"] = self._hmp
        self.handlers["query-mice"] = lambda ctx, args: [
            {"name": "QEMU PS/2 Mouse", "index": 0, "current": True, "absolute": False}]
        self.last_id = {}
        self.server = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.server.bind(self.path)
        self.server.listen(4)
        self.stopped = False
        self.thread = threading.Thread(target=self._accept, daemon=True)
        self.thread.start()

    def on(self, name, fn):
        self.handlers[name] = fn

    def on_hmp(self, prefix, fn):
        self.hmp_handlers[prefix] = fn

    def commands(self, name):
        return [a for _t, n, a in self.log if n == name]

    def times(self, name):
        return [t for t, n, _a in self.log if n == name]

    def _hmp(self, ctx, args):
        line = args["command-line"]
        self.hmp_log.append(line)
        prefix = line.split(" ", 1)[0]
        fn = self.hmp_handlers.get(prefix)
        return fn(ctx, line) if fn else ""

    def _accept(self):
        while not self.stopped:
            try:
                sock, _ = self.server.accept()
            except OSError:
                return
            threading.Thread(target=self._serve, args=(sock,), daemon=True).start()

    def _serve(self, sock):
        conn = Conn(sock)
        buf = b""
        try:
            if self.greeting is not None:
                conn.send(self.greeting)
            while True:
                while b"\n" not in buf:
                    chunk = sock.recv(65536)
                    if not chunk:
                        return
                    buf += chunk
                line, buf = buf.split(b"\n", 1)
                msg = json.loads(line)
                name, args, cid = msg["execute"], msg.get("arguments", {}), msg.get("id")
                self.log.append((time.monotonic(), name, args))
                self.last_id[name] = cid
                handler = self.handlers.get(name)
                try:
                    if handler is None:
                        raise Fail("CommandNotFound")
                    result = handler(conn, args)
                    reply = {"return": {} if result is None else result}
                except Fail as e:
                    reply = {"error": {"class": "GenericError", "desc": str(e)}}
                except Silent:
                    continue
                except Drop:
                    return
                if cid is not None:
                    reply["id"] = cid
                conn.send(reply)
        except OSError:
            pass
        finally:
            sock.close()

    def close(self):
        self.stopped = True
        self.server.close()
        shutil.rmtree(self.dir, ignore_errors=True)
