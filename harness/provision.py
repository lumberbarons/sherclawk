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
    storage.json         each slot's own AFP volume (mount, export, sentinel)
"""
import argparse
import hashlib
import ipaddress
import json
import os
import posixpath
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
STORAGE_NAME = "storage.json"
VOLUME = "Retro68"
SENTINEL = ".harness-slot"
AFP_PORT = 548
# The developer's working export (tools/deploy-to-share.sh); never a slot's.
DEVELOPER_EXPORT = "/srv/retro68"


class ProvisionError(Exception):
    pass


class StorageError(ProvisionError):
    """The slot-to-storage mapping is missing, inconsistent or does not match disk."""


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
        self.storage_path = os.path.join(self.path, STORAGE_NAME)


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
        "-nic", nic_option(s),
        "-display", display,
        "-qmp", "unix:%s,server=on,wait=off" % s.qmp,
        "-pidfile", s.pidfile,
        "-daemonize",
    ]
    return argv


def nic_option(s):
    """The slot's user-mode NIC, with its own AFP server wired in when mapped.

    The guests' startup mounts name the share host by address and the standard
    AFP port, and an AppleShare alias keeps no other port. The guest therefore
    gets a virtual network that contains that address, and QEMU forwards the
    address's AFP port to this slot's own server instance. The developer share
    on the same address is unreachable from the guest, with no change to the
    guest image.
    """
    option = "user,model=sungem,mac=%s" % s.mac
    storage = read_storage(s.home)
    if storage is None or not storage.server:
        return option
    port = storage.slot(s.name).afp_port
    if not port:
        return option
    return "%s,net=%s,guestfwd=tcp:%s:%d-cmd:nc %s %d" % (
        option, storage.network, storage.server, AFP_PORT, storage.server, port)


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


def slot_base(s):
    configured = os.path.join(s.home.base_dir, s.name + ".qcow2")
    return configured if os.path.exists(configured) or s.name in read_manifest(s.home).get("controller_slots", {}) else s.home.base_path


def verify_slot_base(s):
    base = slot_base(s)
    if base == s.home.base_path:
        return
    want = read_manifest(s.home).get("controller_slots", {}).get(s.name)
    if not want or not os.path.exists(base) or not is_read_only(base) or sha256(base) != want:
        raise ProvisionError("configured slot image changed or is not recorded: %s" % base)


def freeze_slot(s):
    """Seal the stopped, controller-configured overlay as an immutable slot base.

    This preserves its shared backing file and installed private slot identity.
    `init` then makes a new runtime overlay; reset never removes the slot base.
    """
    dest = os.path.join(s.home.base_dir, s.name + ".qcow2")
    if pid_of(s):
        raise ProvisionError("shut the guest down cleanly before freezing its slot")
    if os.path.exists(dest) or not os.path.exists(s.overlay):
        raise ProvisionError("slot base already exists or no configured overlay")
    os.replace(s.overlay, dest)
    freeze(dest)
    slots = read_manifest(s.home).get("controller_slots", {})
    slots[s.name] = sha256(dest)
    write_manifest(s.home, {"controller_slots": slots})


def create_overlay(s, qemu_img=QEMU_IMG):
    base = slot_base(s)
    verify_slot_base(s)
    if not os.path.exists(base):
        raise ProvisionError("no base image at %s; run `freeze` first" % base)
    if not is_read_only(base):
        raise ProvisionError("base image %s is writable; refusing to build on it" % base)
    if os.path.exists(s.overlay):
        raise ProvisionError("%s already exists; `reset` it explicitly" % s.overlay)
    os.makedirs(s.home.slot_dir, exist_ok=True)
    result = subprocess.run(overlay_argv(base, s.overlay, qemu_img),
                            capture_output=True, text=True)
    if result.returncode:
        raise ProvisionError("qemu-img could not create %s: %s"
                             % (s.overlay, (result.stderr or result.stdout).strip()))


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
        if os.path.exists(os.path.join(home.base_dir, name + ".qcow2")):
            raise ProvisionError("configured slot bases still name the shared base")
        if os.path.exists(s.overlay):
            raise ProvisionError("%s still overlays the base; `reset %s` first" % (s.overlay, name))
    os.replace(home.work_path, home.base_path)
    freeze(home.base_path)
    # Explicit base replacement retires the archived slots; a missing image
    # remains an error until this transition succeeds.
    write_manifest(home, {
        "qemu": qemu_version(),
        "base_sha256": sha256(home.base_path),
        "original_sha256": sha256(home.original_path),
        "controller_slots": {},
    })


class SlotStorage:
    """One slot's AFP volume: where the host mounts it and where the server keeps it.

    The guest sees `volume` (`Retro68`); `mount` is the host's view of that
    volume and `export` the share host's directory behind it. A sentinel file
    named for the slot at the volume root ties both views to one slot, so a
    mount of another slot's volume or of the developer share is detected.
    """

    def __init__(self, slot, mount, export, volume=VOLUME, isolated=True, afp_port=None):
        self.slot = slot
        self.mount = mount
        self.export = export
        self.volume = volume
        self.isolated = isolated
        self.afp_port = afp_port

    def describe(self):
        return {"slot": self.slot, "volume": self.volume, "mount": self.mount,
                "export": self.export, "isolated": self.isolated,
                "afp_port": self.afp_port}

    def verify_mount(self):
        """Require the mount to be this slot's own volume; legacy storage is exempt."""
        if not os.path.isdir(self.mount):
            raise StorageError("%s volume is not mounted: %s" % (self.slot, self.mount))
        if not self.isolated:
            return
        found = read_sentinel(self.mount)
        if found != self.slot:
            raise StorageError(
                "%s mount %s is %s, not this slot's volume (expected %s in %s)"
                % (self.slot, self.mount,
                   "slot %r" % found if found else "unmarked",
                   self.slot, SENTINEL))


class Storage:
    """The explicit slot-to-storage mapping, one disjoint volume per slot."""

    def __init__(self, slots, isolated=True, server=None):
        self.slots = slots
        self.isolated = isolated
        self.server = server  # address the guests' startup mounts name

    @property
    def network(self):
        return str(ipaddress.ip_interface("%s/24" % self.server).network)

    @classmethod
    def single(cls, mount, export=DEVELOPER_EXPORT):
        """Every slot on one shared root: the unisolated layout, for host tests only."""
        mount = os.path.abspath(mount)
        return cls({n: SlotStorage(n, mount, export, isolated=False) for n in SLOTS},
                   isolated=False)

    def slot(self, name):
        try:
            return self.slots[name]
        except KeyError:
            raise StorageError("no storage mapped for slot %r" % (name,))


def read_sentinel(directory):
    try:
        with open(os.path.join(directory, SENTINEL)) as f:
            return f.read().strip()
    except (FileNotFoundError, NotADirectoryError):
        return None
    except (OSError, UnicodeDecodeError):
        return None


def _nested(a, b):
    a, b = a.rstrip("/") + "/", b.rstrip("/") + "/"
    return a.startswith(b) or b.startswith(a)


def check_server(server):
    try:
        address = ipaddress.IPv4Address(server)
    except ValueError:
        raise StorageError("server %r is not an IPv4 address" % (server,))
    # The guest's virtual network is that address's /24; keep clear of the
    # addresses QEMU assigns to its gateway, DNS and DHCP range.
    last = address.packed[-1]
    if last in (0, 1, 2, 3, 255) or 15 <= last <= 30:
        raise StorageError("server %s collides with the guest network's own addresses" % server)


def check_storage_entries(entries, server=None):
    """Raise unless every slot has its own mount and export, disjoint from the rest."""
    ports = [e["afp_port"] for e in entries.values() if e.get("afp_port")]
    if ports and not server:
        raise StorageError("afp_port needs a server address")
    if server:
        check_server(server)
    if len(ports) != len(set(ports)):
        raise StorageError("slots must serve AFP on different ports")
    for port in ports:
        if not isinstance(port, int) or not 1024 <= port <= 65535 or port == AFP_PORT:
            raise StorageError("afp_port %r must be an unprivileged port other than %d"
                               % (port, AFP_PORT))
    for name, entry in entries.items():
        if name not in SLOTS:
            raise StorageError("unknown slot %r in storage map" % name)
        if not os.path.isabs(entry["mount"]):
            raise StorageError("%s mount must be an absolute path" % name)
        if not posixpath.isabs(entry["export"]):
            raise StorageError("%s export must be an absolute path" % name)
        if _nested(entry["export"], DEVELOPER_EXPORT):
            raise StorageError("%s export %s overlaps the developer share %s"
                               % (name, entry["export"], DEVELOPER_EXPORT))
    names = list(entries)
    for i, a in enumerate(names):
        for b in names[i + 1:]:
            if _nested(entries[a]["mount"], entries[b]["mount"]):
                raise StorageError("%s and %s mounts overlap" % (a, b))
            if _nested(entries[a]["export"], entries[b]["export"]):
                raise StorageError("%s and %s exports overlap" % (a, b))


def read_storage(home):
    """The recorded mapping, or None when none has been written."""
    try:
        with open(home.storage_path) as f:
            data = json.load(f)
    except FileNotFoundError:
        return None
    except ValueError as exc:
        raise StorageError("%s is not valid JSON: %s" % (home.storage_path, exc))
    slots = data.get("slots") if isinstance(data, dict) else None
    if not isinstance(slots, dict) or set(slots) != set(SLOTS):
        raise StorageError("%s must map exactly %s" % (home.storage_path, ", ".join(SLOTS)))
    entries = {}
    for name, entry in slots.items():
        if not isinstance(entry, dict) or not all(
                isinstance(entry.get(k), str) and entry[k] for k in ("mount", "export")):
            raise StorageError("%s needs a mount and an export" % name)
        entries[name] = {"mount": os.path.normpath(entry["mount"]),
                         "export": posixpath.normpath(entry["export"]),
                         "afp_port": entry.get("afp_port")}
    server = data.get("server")
    check_storage_entries(entries, server)
    volume = data.get("volume", VOLUME)
    return Storage({n: SlotStorage(n, e["mount"], e["export"], volume, afp_port=e["afp_port"])
                    for n, e in entries.items()}, server=server)


def load_storage(home, share=None):
    """The mapping a harness command must use.

    The recorded map wins. A bare `share` is the unisolated layout and exists so
    host tests can stand in a single directory; commands never pass one.
    """
    storage = read_storage(home)
    if storage is not None:
        return storage
    if share:
        return Storage.single(share)
    return None


def require_storage(storage):
    if storage is None:
        raise StorageError(
            "no slot storage is mapped; run `provision.py storage set <slot> --mount M "
            "--export E` for each slot (harness/docs/provisioning.md)")
    return storage


def set_storage(home, name, mount, export, afp_port=None, server=None):
    """Record one slot's volume, refusing any mapping that overlaps another's."""
    slot(home, name)
    data = {}
    try:
        with open(home.storage_path) as f:
            data = json.load(f)
    except FileNotFoundError:
        pass
    slots = dict(data.get("slots", {}))
    slots[name] = {"mount": os.path.normpath(os.path.abspath(os.path.expanduser(mount))),
                   "export": posixpath.normpath(export)}
    if afp_port:
        slots[name]["afp_port"] = afp_port
    server = server or data.get("server")
    check_storage_entries({n: dict(e, afp_port=e.get("afp_port")) for n, e in slots.items()},
                          server)
    os.makedirs(home.path, exist_ok=True)
    tmp = home.storage_path + ".tmp"
    document = {"volume": VOLUME, "slots": slots}
    if server:
        document["server"] = server
    with open(tmp, "w") as f:
        json.dump(document, f, indent=2, sort_keys=True)
        f.write("\n")
    os.replace(tmp, home.storage_path)


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


def storage_command(home, args):
    if args.storage_cmd == "set":
        set_storage(home, args.slot, args.mount, args.export, args.afp_port, args.server)
        return
    storage = require_storage(read_storage(home))
    if args.storage_cmd == "check":
        for name in SLOTS:
            storage.slot(name).verify_mount()
    json.dump({n: storage.slot(n).describe() for n in SLOTS}, sys.stdout,
              indent=2, sort_keys=True)
    print()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--home", help="harness state directory (default $OS9_HARNESS_HOME)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("info", help="print emulator version, base checksums and slot identities")
    sub.add_parser("freeze", help="make the working image the immutable base and record it")
    for name, text in (("freeze-slot", "seal a stopped controller-configured slot image"),
                       ("init", "create the slot overlay"),
                       ("stop", "stop the slot's QEMU"),
                       ("reset", "stop the slot and delete its overlay")):
        sub.add_parser(name, help=text).add_argument("slot")
    p = sub.add_parser("storage", help="show, set or check each slot's own AFP volume")
    st = p.add_subparsers(dest="storage_cmd", required=True)
    st.add_parser("show", help="print the slot-to-storage mapping")
    st.add_parser("check", help="require every mounted volume to carry its own slot's sentinel")
    q = st.add_parser("set", help="map one slot to a host mount and a server export")
    q.add_argument("slot")
    q.add_argument("--mount", required=True, help="host mount point of the slot's volume")
    q.add_argument("--export", required=True, help="share-host directory behind the volume")
    q.add_argument("--afp-port", type=int,
                   help="port of the slot's own AFP instance; the guest's AFP traffic is forwarded to it")
    q.add_argument("--server", help="address the guests' startup mounts name (needed with --afp-port)")
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
            storage = read_storage(home)
            info = {"qemu": qemu_version(), "manifest": read_manifest(home),
                    "slots": {n: {"mac": slot(home, n).mac, "qmp": slot(home, n).qmp,
                                  "overlay": slot(home, n).overlay,
                                  "storage": storage.slot(n).describe() if storage else None}
                              for n in SLOTS}}
            json.dump(info, sys.stdout, indent=2, sort_keys=True)
            print()
        elif args.cmd == "freeze":
            promote_work(home)
        elif args.cmd == "freeze-slot":
            freeze_slot(slot(home, args.slot))
        elif args.cmd == "init":
            create_overlay(slot(home, args.slot))
        elif args.cmd == "stop":
            stop(slot(home, args.slot))
        elif args.cmd == "reset":
            s = slot(home, args.slot)
            stop(s)
            if os.path.exists(s.overlay):
                os.remove(s.overlay)
        elif args.cmd == "storage":
            storage_command(home, args)
        elif args.cmd == "launch":
            verify_base(home)
            s = slot(home, args.slot)
            verify_slot_base(s)
            print(launch(s, args.display, args.cdrom))
        elif args.cmd == "launch-work":
            print(launch(slot(home, args.slot), args.display, args.cdrom, drive=home.work_path))
    except ProvisionError as e:
        print("provision: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
