#!/usr/bin/env python3
"""Provision standalone QEMU guests for Mac OS 9 diagnostics.

One immutable base image, one copy-on-write overlay per slot. A slot is a
named identity (MAC address, QMP socket, overlay, pid and log files) that can
be reset without touching the base or any other slot. The original QEMU
install and the UTM virtual machine are never opened for writing.

Stdlib only, and no imports from tools/ or src/ (ADR-0009). State lives in
$OS9_HARNESS_HOME (default ~/.local/share/os9-harness), outside any worktree:

    base/os9-mpw.qcow2   frozen guest: Mac OS 9.2.2 plus MPW/ToolServer
    base/os9-original.qcow2   verbatim copy of the original QEMU install
    slots/<slot>.qcow2   per-slot overlay on the base
    run/<slot>.{qmp,pid,log}
    manifest.json        emulator version and base checksums
"""
import argparse
import hashlib
import json
import os
import re
import signal
import subprocess
import sys
import time

SLOTS = ("slot1", "slot2")
BASE_NAME = "os9-mpw.qcow2"
ORIGINAL_NAME = "os9-original.qcow2"
WORK_NAME = "golden-work.qcow2"
QEMU = "qemu-system-ppc"
QEMU_IMG = "qemu-img"


class ProvisionError(Exception):
    pass


class Home:
    def __init__(self, path=None):
        self.path = path or os.environ.get("OS9_HARNESS_HOME") or os.path.expanduser(
            "~/.local/share/os9-harness")
        self.base_dir = os.path.join(self.path, "base")
        self.slot_dir = os.path.join(self.path, "slots")
        self.run_dir = os.path.join(self.path, "run")
        self.base_path = os.path.join(self.base_dir, BASE_NAME)
        self.original_path = os.path.join(self.base_dir, ORIGINAL_NAME)
        self.work_path = os.path.join(self.base_dir, WORK_NAME)
        self.manifest_path = os.path.join(self.path, "manifest.json")


class Slot:
    def __init__(self, home, name, index):
        self.home = home
        self.name = name
        self.index = index
        # Locally administered, unicast (first octet 0x52); the last octet
        # is the slot number.
        self.mac = "52:54:00:09:00:%02x" % index
        self.overlay = os.path.join(home.slot_dir, name + ".qcow2")
        self.qmp = os.path.join(home.run_dir, name + ".qmp")
        self.pidfile = os.path.join(home.run_dir, name + ".pid")
        self.log = os.path.join(home.run_dir, name + ".log")


def slot(home, name):
    if name not in SLOTS:
        raise ProvisionError("unknown slot %r (expected one of %s)" % (name, ", ".join(SLOTS)))
    return Slot(home, name, SLOTS.index(name) + 1)


def qemu_argv(s, qemu=QEMU, display="none", cdroms=(), drive=None):
    """The original launcher's machine, plus a slot identity and QMP socket.

    `drive` overrides the boot disk (used to boot the writable working image
    while the base is being built); slots always boot their overlay.
    """
    argv = [
        qemu,
        "-M", "mac99",
        "-m", "512",
        "-rtc", "base=localtime",
        "-drive", "file=%s,format=qcow2,media=disk" % (drive or s.overlay),
    ]
    for iso in cdroms:
        argv += ["-drive", "file=%s,format=raw,media=cdrom,readonly=on" % iso]
    argv += [
        "-device", "usb-kbd",
        "-device", "usb-mouse",
        "-nic", "user,model=sungem,mac=%s" % s.mac,
        "-display", display,
        "-qmp", "unix:%s,server=on,wait=off" % s.qmp,
        "-pidfile", s.pidfile,
        "-daemonize",
    ]
    return argv


def overlay_argv(base, overlay, qemu_img=QEMU_IMG):
    return [qemu_img, "create", "-f", "qcow2", "-b", base, "-F", "qcow2", overlay]


def parse_qemu_version(text):
    m = re.search(r"version (\d+\.\d+(?:\.\d+)?)", text)
    if not m:
        raise ProvisionError("not a QEMU version banner: %r" % text[:60])
    return m.group(1)


def qemu_version(qemu=QEMU):
    out = subprocess.run([qemu, "--version"], capture_output=True, text=True, check=False)
    return parse_qemu_version(out.stdout)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def is_read_only(path):
    return os.stat(path).st_mode & 0o222 == 0


def freeze(path):
    os.chmod(path, os.stat(path).st_mode & ~0o222)


def create_overlay(s, qemu_img=QEMU_IMG):
    base = s.home.base_path
    if not os.path.exists(base):
        raise ProvisionError("no base image at %s; run `freeze` first" % base)
    if not is_read_only(base):
        raise ProvisionError("base image %s is writable; refusing to build on it" % base)
    if os.path.exists(s.overlay):
        raise ProvisionError("%s already exists; `reset` it explicitly" % s.overlay)
    os.makedirs(s.home.slot_dir, exist_ok=True)
    subprocess.run(overlay_argv(base, s.overlay, qemu_img), check=True)


def read_manifest(home):
    try:
        with open(home.manifest_path) as f:
            return json.load(f)
    except FileNotFoundError:
        return {}


def write_manifest(home, updates):
    data = read_manifest(home)
    data.update(updates)
    os.makedirs(home.path, exist_ok=True)
    tmp = home.manifest_path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(data, f, indent=2, sort_keys=True)
        f.write("\n")
    os.replace(tmp, home.manifest_path)


def verify_base(home):
    want = read_manifest(home).get("base_sha256")
    if not want:
        raise ProvisionError("manifest records no base checksum")
    got = sha256(home.base_path)
    if got != want:
        raise ProvisionError("base image changed: manifest %s, file %s" % (want, got))


def promote_work(home, qemu_version=qemu_version):
    """Make the working image the immutable base and record what built it."""
    if os.path.exists(home.base_path):
        raise ProvisionError("%s exists; the base is never replaced" % home.base_path)
    if not os.path.exists(home.work_path):
        raise ProvisionError("no working image at %s" % home.work_path)
    for name in SLOTS:
        s = slot(home, name)
        if pid_of(s):
            raise ProvisionError("stop every guest before freezing")
        if os.path.exists(s.overlay):
            raise ProvisionError("%s still overlays the base; `reset %s` first" % (s.overlay, name))
    os.replace(home.work_path, home.base_path)
    freeze(home.base_path)
    write_manifest(home, {
        "qemu": qemu_version(),
        "base_sha256": sha256(home.base_path),
        "original_sha256": sha256(home.original_path),
    })


def pid_of(s):
    try:
        with open(s.pidfile) as f:
            pid = int(f.read().strip())
        os.kill(pid, 0)
        return pid
    except (FileNotFoundError, ValueError, ProcessLookupError):
        return None


def launch(s, display="none", cdroms=(), drive=None, qemu=QEMU):
    if pid_of(s):
        raise ProvisionError("%s is already running (pid %d)" % (s.name, pid_of(s)))
    os.makedirs(s.home.run_dir, exist_ok=True)
    for stale in (s.qmp, s.pidfile):
        if os.path.exists(stale):
            os.remove(stale)
    if drive is None and not os.path.exists(s.overlay):
        raise ProvisionError("no overlay for %s; run `init %s`" % (s.name, s.name))
    argv = qemu_argv(s, qemu=qemu, display=display, cdroms=cdroms, drive=drive)
    with open(s.log, "ab") as log:
        subprocess.run(argv, stdout=log, stderr=log, check=True)
    for _ in range(50):
        if os.path.exists(s.qmp) and pid_of(s):
            return pid_of(s)
        time.sleep(0.1)
    raise ProvisionError("%s did not come up; see %s" % (s.name, s.log))


def stop(s, timeout=10.0):
    pid = pid_of(s)
    if not pid:
        return False
    os.kill(pid, signal.SIGTERM)
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not pid_of(s):
            return True
        time.sleep(0.1)
    raise ProvisionError("%s (pid %d) ignored SIGTERM" % (s.name, pid))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--home", help="harness state directory (default $OS9_HARNESS_HOME)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info", help="print emulator version, base checksums and slot identities")
    sub.add_parser("freeze", help="make the working image the immutable base and record it")
    for name, text in (("init", "create the slot overlay"),
                       ("stop", "stop the slot's QEMU"),
                       ("reset", "stop the slot and delete its overlay")):
        sub.add_parser(name, help=text).add_argument("slot")
    p = sub.add_parser("launch", help="start the slot's guest headless (or windowed)")
    p.add_argument("slot")
    p.add_argument("--display", default="none", help="QEMU -display (default none)")
    p.add_argument("--cdrom", action="append", default=[], help="read-only CD image to attach")
    p = sub.add_parser("launch-work", help="boot the writable working image to prepare the base")
    p.add_argument("slot", nargs="?", default="slot1")
    p.add_argument("--display", default="none")
    p.add_argument("--cdrom", action="append", default=[])
    args = ap.parse_args(argv)
    home = Home(args.home)

    try:
        if args.cmd == "info":
            info = {"qemu": qemu_version(), "manifest": read_manifest(home),
                    "slots": {n: {"mac": slot(home, n).mac, "qmp": slot(home, n).qmp,
                                  "overlay": slot(home, n).overlay} for n in SLOTS}}
            json.dump(info, sys.stdout, indent=2, sort_keys=True)
            print()
        elif args.cmd == "freeze":
            promote_work(home)
        elif args.cmd == "init":
            create_overlay(slot(home, args.slot))
        elif args.cmd == "stop":
            stop(slot(home, args.slot))
        elif args.cmd == "reset":
            s = slot(home, args.slot)
            stop(s)
            if os.path.exists(s.overlay):
                os.remove(s.overlay)
        elif args.cmd == "launch":
            verify_base(home)
            print(launch(slot(home, args.slot), args.display, args.cdrom))
        elif args.cmd == "launch-work":
            print(launch(slot(home, args.slot), args.display, args.cdrom, drive=home.work_path))
    except ProvisionError as e:
        print("provision: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
