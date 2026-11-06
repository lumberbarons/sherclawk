"""Host tests for guest provisioning: no QEMU, VM or network needed."""
import contextlib
import hashlib
import io
import json
import os
import stat
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import fake_qmp  # noqa: E402
import provision  # noqa: E402


def fake_tool(directory, name, body):
    path = os.path.join(directory, name)
    with open(path, "w") as f:
        f.write("#!/bin/sh\n" + body + "\n")
    os.chmod(path, 0o755)
    return path


class SlotIdentityTest(unittest.TestCase):
    def setUp(self):
        self.home = provision.Home("/h")

    def test_slots_have_distinct_identities(self):
        a = provision.slot(self.home, "slot1")
        b = provision.slot(self.home, "slot2")
        self.assertNotEqual(a.mac, b.mac)
        self.assertNotEqual(a.qmp, b.qmp)
        self.assertNotEqual(a.overlay, b.overlay)
        self.assertNotEqual(a.pidfile, b.pidfile)

    def test_mac_is_locally_administered_unicast(self):
        for name in provision.SLOTS:
            first = int(provision.slot(self.home, name).mac.split(":")[0], 16)
            self.assertEqual(first & 0x03, 0x02)

    def test_unknown_slot_is_rejected(self):
        with self.assertRaises(provision.ProvisionError):
            provision.slot(self.home, "slot9")

    def test_slot_paths_stay_under_home(self):
        s = provision.slot(self.home, "slot1")
        for p in (s.overlay, s.qmp, s.pidfile, s.log):
            self.assertTrue(p.startswith("/h/"), p)


class QemuArgvTest(unittest.TestCase):
    def setUp(self):
        self.home = provision.Home("/h")
        self.slot = provision.slot(self.home, "slot1")
        self.argv = provision.qemu_argv(self.slot)

    def pair(self, flag):
        i = self.argv.index(flag)
        return self.argv[i + 1]

    def test_keeps_the_original_machine_baseline(self):
        self.assertEqual(self.pair("-M"), "mac99")
        self.assertEqual(self.pair("-m"), "512")
        self.assertEqual(self.pair("-rtc"), "base=localtime")
        self.assertIn("usb-kbd", self.argv)
        self.assertIn("usb-mouse", self.argv)

    def test_boots_the_slot_overlay_only(self):
        drives = [a for a in self.argv if a.startswith("file=")]
        self.assertEqual(len(drives), 1)
        self.assertIn("file=" + self.slot.overlay, drives[0])
        self.assertIn("format=qcow2", drives[0])
        self.assertNotIn(provision.BASE_NAME, " ".join(self.argv))

    def test_network_uses_the_slot_mac(self):
        nic = self.pair("-nic")
        self.assertIn("model=sungem", nic)
        self.assertIn("mac=" + self.slot.mac, nic)
        self.assertTrue(nic.startswith("user"))

    def test_is_headless_with_a_private_qmp_socket(self):
        self.assertEqual(self.pair("-display"), "none")
        self.assertIn("unix:" + self.slot.qmp, self.pair("-qmp"))
        self.assertIn("-daemonize", self.argv)
        self.assertEqual(self.pair("-pidfile"), self.slot.pidfile)

    def test_display_can_be_a_window_and_cdroms_attach_read_only(self):
        argv = provision.qemu_argv(self.slot, display="cocoa", cdroms=["/x/a.iso"])
        self.assertEqual(argv[argv.index("-display") + 1], "cocoa")
        cd = [a for a in argv if "media=cdrom" in a]
        self.assertEqual(len(cd), 1)
        self.assertIn("file=/x/a.iso", cd[0])
        self.assertIn("readonly=on", cd[0])

    def test_distinct_slots_never_share_a_drive_or_socket(self):
        other = provision.qemu_argv(provision.slot(self.home, "slot2"))
        mine = {a for a in self.argv if a.startswith(("file=", "unix:"))}
        theirs = {a for a in other if a.startswith(("file=", "unix:"))}
        self.assertFalse(any(m.split(",")[0] == t.split(",")[0]
                             for m in mine for t in theirs))


class VersionTest(unittest.TestCase):
    def test_parses_the_qemu_banner(self):
        text = "QEMU emulator version 10.1.2\nCopyright (c) 2003-2025 Fabrice Bellard"
        self.assertEqual(provision.parse_qemu_version(text), "10.1.2")

    def test_rejects_other_output(self):
        with self.assertRaises(provision.ProvisionError):
            provision.parse_qemu_version("command not found")


class OverlayTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = provision.Home(self.tmp.name)
        os.makedirs(self.home.base_dir)
        self.log = os.path.join(self.tmp.name, "calls")
        self.qemu_img = fake_tool(
            self.tmp.name, "qemu-img",
            'echo "$@" >> "%s"\nfor last; do :; done\n: > "$last"' % self.log)
        self.base = self.home.base_path
        with open(self.base, "wb") as f:
            f.write(b"base image")
        os.chmod(self.base, 0o444)

    def calls(self):
        with open(self.log) as f:
            return f.read().splitlines()

    def test_creates_a_backed_overlay_without_touching_the_base(self):
        before = provision.sha256(self.base)
        s = provision.slot(self.home, "slot1")
        provision.create_overlay(s, qemu_img=self.qemu_img)
        self.assertTrue(os.path.exists(s.overlay))
        (call,) = self.calls()
        self.assertIn("-b " + self.base, call)
        self.assertIn("-F qcow2", call)
        self.assertEqual(before, provision.sha256(self.base))

    def test_refuses_a_writable_base(self):
        os.chmod(self.base, 0o644)
        with self.assertRaises(provision.ProvisionError):
            provision.create_overlay(provision.slot(self.home, "slot1"), qemu_img=self.qemu_img)

    def test_refuses_a_missing_base(self):
        os.chmod(self.base, 0o644)
        os.remove(self.base)
        with self.assertRaises(provision.ProvisionError):
            provision.create_overlay(provision.slot(self.home, "slot1"), qemu_img=self.qemu_img)

    def test_never_replaces_an_existing_overlay(self):
        s = provision.slot(self.home, "slot1")
        os.makedirs(os.path.dirname(s.overlay), exist_ok=True)
        with open(s.overlay, "wb") as f:
            f.write(b"guest state")
        with self.assertRaises(provision.ProvisionError):
            provision.create_overlay(s, qemu_img=self.qemu_img)
        with open(s.overlay, "rb") as f:
            self.assertEqual(f.read(), b"guest state")

    def test_qemu_img_failure_is_reported_with_stderr(self):
        broken = fake_tool(self.tmp.name, "qemu-img-broken",
                           'echo "backing file changed" >&2\nexit 3')
        with self.assertRaisesRegex(provision.ProvisionError, "backing file changed"):
            provision.create_overlay(provision.slot(self.home, "slot1"), qemu_img=broken)

    def test_freeze_removes_every_write_bit(self):
        os.chmod(self.base, 0o644)
        provision.freeze(self.base)
        self.assertEqual(stat.S_IMODE(os.stat(self.base).st_mode) & 0o222, 0)


class PromoteTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = provision.Home(self.tmp.name)
        os.makedirs(self.home.base_dir)
        for path, data in ((self.home.work_path, b"work"), (self.home.original_path, b"orig")):
            with open(path, "wb") as f:
                f.write(data)
        os.chmod(self.home.original_path, 0o444)

    def test_promotes_the_working_image_and_records_checksums(self):
        provision.promote_work(self.home, qemu_version=lambda: "10.1.2")
        self.assertFalse(os.path.exists(self.home.work_path))
        self.assertTrue(provision.is_read_only(self.home.base_path))
        m = provision.read_manifest(self.home)
        self.assertEqual(m["base_sha256"], hashlib.sha256(b"work").hexdigest())
        self.assertEqual(m["original_sha256"], hashlib.sha256(b"orig").hexdigest())
        self.assertEqual(m["qemu"], "10.1.2")

    def test_never_replaces_an_existing_base(self):
        with open(self.home.base_path, "wb") as f:
            f.write(b"older base")
        with self.assertRaises(provision.ProvisionError):
            provision.promote_work(self.home, qemu_version=lambda: "10.1.2")
        with open(self.home.base_path, "rb") as f:
            self.assertEqual(f.read(), b"older base")
        self.assertTrue(os.path.exists(self.home.work_path))

    def test_replacement_retires_archived_configured_slots_before_init(self):
        os.makedirs(self.home.slot_dir)
        for name in provision.SLOTS:
            s = provision.slot(self.home, name)
            with open(s.overlay, "wb") as f:
                f.write(b"old configured slot")
            provision.freeze_slot(s)
            configured = provision.slot_base(s)
            os.rename(configured, configured + ".archived")
        provision.promote_work(self.home, qemu_version=lambda: "10.1.2")
        self.assertEqual(provision.read_manifest(self.home)["controller_slots"], {})
        qemu_img = fake_tool(self.tmp.name, "qemu-img", 'for last; do :; done\n: > "$last"')
        for name in provision.SLOTS:
            s = provision.slot(self.home, name)
            self.assertEqual(provision.slot_base(s), self.home.base_path)
            provision.create_overlay(s, qemu_img=qemu_img)
            self.assertTrue(os.path.exists(s.overlay))

    def test_refuses_to_retire_a_configured_image_that_still_exists(self):
        os.makedirs(self.home.slot_dir)
        s = provision.slot(self.home, "slot1")
        with open(s.overlay, "wb") as f:
            f.write(b"configured slot")
        provision.freeze_slot(s)
        before = provision.read_manifest(self.home)
        with self.assertRaises(provision.ProvisionError):
            provision.promote_work(self.home, qemu_version=lambda: "10.1.2")
        self.assertEqual(provision.read_manifest(self.home), before)
        self.assertTrue(os.path.exists(self.home.work_path))

    def test_refuses_while_a_slot_overlay_still_names_the_base(self):
        s = provision.slot(self.home, "slot2")
        os.makedirs(self.home.slot_dir)
        with open(s.overlay, "wb") as f:
            f.write(b"overlay")
        with self.assertRaises(provision.ProvisionError):
            provision.promote_work(self.home, qemu_version=lambda: "10.1.2")
        self.assertTrue(os.path.exists(self.home.work_path))

    def test_refuses_without_a_working_image(self):
        os.remove(self.home.work_path)
        with self.assertRaises(provision.ProvisionError):
            provision.promote_work(self.home, qemu_version=lambda: "10.1.2")


class ManifestTest(unittest.TestCase):
    def test_sha256_matches_hashlib(self):
        with tempfile.NamedTemporaryFile() as f:
            f.write(b"x" * 100000)
            f.flush()
            self.assertEqual(provision.sha256(f.name),
                             hashlib.sha256(b"x" * 100000).hexdigest())

    def test_verify_base_flags_a_changed_image(self):
        with tempfile.TemporaryDirectory() as d:
            home = provision.Home(d)
            os.makedirs(home.base_dir)
            with open(home.base_path, "wb") as f:
                f.write(b"one")
            provision.write_manifest(home, {"base_sha256": provision.sha256(home.base_path)})
            provision.verify_base(home)
            os.chmod(home.base_path, 0o644)
            with open(home.base_path, "wb") as f:
                f.write(b"two")
            with self.assertRaises(provision.ProvisionError):
                provision.verify_base(home)


class StorageMapTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = provision.Home(os.path.join(self.tmp.name, "home"))
        self.mounts = {n: os.path.join(self.tmp.name, "mnt-" + n) for n in provision.SLOTS}
        for path in self.mounts.values():
            os.makedirs(path)

    def configure(self):
        for n in provision.SLOTS:
            provision.set_storage(self.home, n, self.mounts[n], "/srv/harness/" + n)

    def mark(self, name, content=None):
        with open(os.path.join(self.mounts[name], provision.SENTINEL), "w") as f:
            f.write((content or name) + "\n")

    def test_no_map_is_not_storage(self):
        self.assertIsNone(provision.read_storage(self.home))
        with self.assertRaisesRegex(provision.StorageError, "storage set"):
            provision.require_storage(provision.load_storage(self.home))

    def test_each_slot_gets_its_own_mount_and_export(self):
        self.configure()
        storage = provision.read_storage(self.home)
        one, two = storage.slot("slot1"), storage.slot("slot2")
        self.assertEqual(one.volume, "Retro68")
        self.assertNotEqual(one.mount, two.mount)
        self.assertNotEqual(one.export, two.export)
        self.assertEqual(one.export, "/srv/harness/slot1")
        self.assertTrue(one.isolated)

    def test_incomplete_map_is_refused(self):
        provision.set_storage(self.home, "slot1", self.mounts["slot1"], "/srv/harness/slot1")
        with self.assertRaisesRegex(provision.StorageError, "exactly slot1, slot2"):
            provision.read_storage(self.home)

    def test_overlapping_exports_and_mounts_are_refused(self):
        provision.set_storage(self.home, "slot1", self.mounts["slot1"], "/srv/harness/slot1")
        with self.assertRaisesRegex(provision.StorageError, "exports overlap"):
            provision.set_storage(self.home, "slot2", self.mounts["slot2"],
                                  "/srv/harness/slot1/inner")
        with self.assertRaisesRegex(provision.StorageError, "exports overlap"):
            provision.set_storage(self.home, "slot2", self.mounts["slot2"], "/srv/harness/slot1")
        with self.assertRaisesRegex(provision.StorageError, "mounts overlap"):
            provision.set_storage(self.home, "slot2", self.mounts["slot1"], "/srv/harness/slot2")
        self.assertFalse(os.path.exists(self.home.storage_path + ".tmp"))

    def test_developer_share_is_never_a_slot_export(self):
        for export in ("/srv/retro68", "/srv/retro68/slot1", "/srv"):
            with self.assertRaisesRegex(provision.StorageError, "developer share"):
                provision.set_storage(self.home, "slot1", self.mounts["slot1"], export)

    def test_relative_and_unknown_inputs_are_refused(self):
        with self.assertRaisesRegex(provision.StorageError, "absolute"):
            provision.set_storage(self.home, "slot1", self.mounts["slot1"], "srv/slot1")
        with self.assertRaises(provision.ProvisionError):
            provision.set_storage(self.home, "slot9", self.mounts["slot1"], "/srv/h/slot9")

    def test_mount_must_carry_its_own_slots_sentinel(self):
        self.configure()
        one = provision.read_storage(self.home).slot("slot1")
        with self.assertRaisesRegex(provision.StorageError, "unmarked"):
            one.verify_mount()
        self.mark("slot1", "slot2")
        with self.assertRaisesRegex(provision.StorageError, "slot 'slot2'"):
            one.verify_mount()
        self.mark("slot1")
        one.verify_mount()

    def test_unmounted_volume_is_refused(self):
        self.configure()
        os.rmdir(self.mounts["slot2"])
        with self.assertRaisesRegex(provision.StorageError, "not mounted"):
            provision.read_storage(self.home).slot("slot2").verify_mount()

    def test_single_root_is_unisolated_and_skips_the_sentinel(self):
        storage = provision.load_storage(self.home, share=self.mounts["slot1"])
        self.assertFalse(storage.isolated)
        storage.slot("slot2").verify_mount()
        self.assertEqual(storage.slot("slot1").mount, storage.slot("slot2").mount)

    def test_recorded_map_wins_over_a_bare_share(self):
        self.configure()
        storage = provision.load_storage(self.home, share=self.mounts["slot1"])
        self.assertTrue(storage.isolated)

    def test_corrupt_map_is_refused(self):
        os.makedirs(self.home.path)
        with open(self.home.storage_path, "w") as f:
            f.write("{")
        with self.assertRaisesRegex(provision.StorageError, "not valid JSON"):
            provision.read_storage(self.home)

    def run_cli(self, argv):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return provision.main(argv)

    def test_cli_sets_shows_and_checks(self):
        base = ["--home", self.home.path, "storage"]
        for n in provision.SLOTS:
            self.assertEqual(self.run_cli(base + ["set", n, "--mount", self.mounts[n],
                                                    "--export", "/srv/harness/" + n]), 0)
        self.assertEqual(self.run_cli(base + ["show"]), 0)
        self.assertEqual(self.run_cli(base + ["check"]), 1)
        self.mark("slot1")
        self.assertEqual(self.run_cli(base + ["check"]), 1)
        self.mark("slot2")
        self.assertEqual(self.run_cli(base + ["check"]), 0)

    def test_a_mapped_slot_forwards_the_guests_afp_traffic_to_its_own_instance(self):
        for n, port in (("slot1", 5481), ("slot2", 5482)):
            provision.set_storage(self.home, n, self.mounts[n], "/srv/harness/" + n,
                                  afp_port=port, server="10.0.4.48")
        nics = {}
        for n in provision.SLOTS:
            argv = provision.qemu_argv(provision.slot(self.home, n))
            nics[n] = argv[argv.index("-nic") + 1]
        self.assertIn("net=10.0.4.0/24", nics["slot1"])
        self.assertIn("guestfwd=tcp:10.0.4.48:548-cmd:nc 10.0.4.48 5481", nics["slot1"])
        self.assertIn("guestfwd=tcp:10.0.4.48:548-cmd:nc 10.0.4.48 5482", nics["slot2"])
        self.assertNotIn("5482", nics["slot1"])
        self.assertIn("mac=52:54:00:09:00:01", nics["slot1"])

    def test_without_a_forward_the_nic_is_the_plain_user_network(self):
        self.configure()
        argv = provision.qemu_argv(provision.slot(self.home, "slot1"))
        self.assertEqual(argv[argv.index("-nic") + 1],
                         "user,model=sungem,mac=52:54:00:09:00:01")

    def test_forwarding_needs_distinct_unprivileged_ports_and_a_usable_server(self):
        mount = self.mounts["slot1"]
        with self.assertRaisesRegex(provision.StorageError, "needs a server"):
            provision.set_storage(self.home, "slot1", mount, "/srv/h/slot1", afp_port=5481)
        with self.assertRaisesRegex(provision.StorageError, "unprivileged"):
            provision.set_storage(self.home, "slot1", mount, "/srv/h/slot1",
                                  afp_port=548, server="10.0.4.48")
        with self.assertRaisesRegex(provision.StorageError, "not an IPv4"):
            provision.set_storage(self.home, "slot1", mount, "/srv/h/slot1",
                                  afp_port=5481, server="beardmore")
        for clash in ("10.0.4.2", "10.0.4.20", "10.0.4.255"):
            with self.assertRaisesRegex(provision.StorageError, "collides"):
                provision.set_storage(self.home, "slot1", mount, "/srv/h/slot1",
                                      afp_port=5481, server=clash)
        provision.set_storage(self.home, "slot1", mount, "/srv/h/slot1",
                              afp_port=5481, server="10.0.4.48")
        with self.assertRaisesRegex(provision.StorageError, "different ports"):
            provision.set_storage(self.home, "slot2", self.mounts["slot2"], "/srv/h/slot2",
                                  afp_port=5481)

    def test_cli_show_without_a_map_fails(self):
        self.assertEqual(self.run_cli(["--home", self.home.path, "storage", "show"]), 1)


if __name__ == "__main__":
    unittest.main()


class ControllerImageTest(unittest.TestCase):
    def test_freeze_slot_preserves_shared_base_and_creates_fresh_overlay(self):
        with tempfile.TemporaryDirectory() as root:
            home = provision.Home(root)
            os.makedirs(home.base_dir)
            os.makedirs(home.slot_dir)
            with open(home.base_path, 'wb') as f:
                f.write(b'immutable shared base')
            provision.freeze(home.base_path)
            s = provision.slot(home, 'slot1')
            with open(s.overlay, 'wb') as f:
                f.write(b'controller installed overlay')
            provision.freeze_slot(s)
            self.assertFalse(os.path.exists(s.overlay))
            self.assertEqual(provision.slot_base(s), os.path.join(home.base_dir, 'slot1.qcow2'))
            self.assertTrue(provision.is_read_only(provision.slot_base(s)))
            with open(home.base_path, 'rb') as f:
                self.assertEqual(f.read(), b'immutable shared base')
            with self.assertRaises(provision.ProvisionError):
                provision.freeze_slot(s)

    def test_slot_image_checksum_is_checked_before_boot(self):
        with tempfile.TemporaryDirectory() as root:
            home = provision.Home(root)
            os.makedirs(home.base_dir)
            os.makedirs(home.slot_dir)
            s = provision.slot(home, 'slot2')
            with open(s.overlay, 'wb') as f:
                f.write(b'configured slot')
            provision.freeze_slot(s)
            os.chmod(provision.slot_base(s), 0o644)
            with open(provision.slot_base(s), 'wb') as f:
                f.write(b'changed')
            provision.freeze(provision.slot_base(s))
            with self.assertRaises(provision.ProvisionError):
                provision.verify_slot_base(s)


    def test_cli_freeze_slot_and_missing_recorded_image(self):
        with tempfile.TemporaryDirectory() as root:
            home = provision.Home(root)
            os.makedirs(home.base_dir)
            os.makedirs(home.slot_dir)
            s = provision.slot(home, 'slot1')
            with open(s.overlay, 'wb') as f:
                f.write(b'configured')
            self.assertEqual(provision.main(['--home', root, 'freeze-slot', 'slot1']), 0)
            configured = provision.slot_base(s)
            self.assertNotEqual(configured, home.base_path)
            os.remove(configured)
            with self.assertRaises(provision.ProvisionError):
                provision.verify_slot_base(s)
            with open(home.base_path, 'wb') as f:
                f.write(b'shared base still exists')
            provision.freeze(home.base_path)
            with self.assertRaises(provision.ProvisionError):
                provision.create_overlay(s)


class Probe:
    """Injectable host facts: which pids are alive and how long they have run."""

    def __init__(self, alive=(), uptimes=None):
        self.alive_pids = set(alive)
        self.uptimes = dict(uptimes or {})

    def alive(self, pid):
        return pid in self.alive_pids

    def uptime(self, pid):
        return self.uptimes.get(pid)


class EtimeTest(unittest.TestCase):
    def test_parses_every_ps_etime_form(self):
        self.assertEqual(provision.parse_etime("03:15"), 195)
        self.assertEqual(provision.parse_etime("01:03:15"), 3795)
        self.assertEqual(provision.parse_etime("2-01:03:15"), 176595)
        self.assertEqual(provision.parse_etime("  64-20:03:54\n"),
                         64 * 86400 + 20 * 3600 + 3 * 60 + 54)

    def test_rejects_garbage(self):
        for text in ("", "soon", "1:2:3:4", "1-"):
            with self.assertRaises(ValueError):
                provision.parse_etime(text)


class UptimeFormatTest(unittest.TestCase):
    def test_formats_the_largest_units_first(self):
        self.assertEqual(provision.format_uptime(None), "-")
        self.assertEqual(provision.format_uptime(6), "6s")
        self.assertEqual(provision.format_uptime(195), "3m15s")
        self.assertEqual(provision.format_uptime(3795), "1h03m15s")
        self.assertEqual(provision.format_uptime(176595), "2d01h03m15s")


class StatusTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.home = provision.Home(self.tmp.name)
        os.makedirs(self.home.run_dir)

    def slot(self, name):
        return provision.slot(self.home, name)

    def qmp_server(self, name):
        server = fake_qmp.FakeQMP()
        self.addCleanup(server.close)
        os.symlink(server.path, self.slot(name).qmp)
        return server

    def record(self, name, pid):
        with open(self.slot(name).pidfile, "w") as f:
            f.write("%d\n" % pid)

    def report(self, probe=None, timeout=1.0):
        return provision.status(self.home, probe=probe or Probe(), qmp_timeout=timeout)

    def run_cli(self, *argv):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            rc = provision.main(["--home", self.home.path, *argv])
        return rc, out.getvalue()

    def test_running_slot_reports_pid_uptime_qmp_and_overlay(self):
        server = self.qmp_server("slot1")
        server.on("query-status", lambda c, a: {"status": "running", "running": True})
        self.record("slot1", 4242)
        report = self.report(Probe({4242}, {4242: 3723}))
        running, stopped = report["slots"]
        self.assertEqual(running, {
            "slot": "slot1", "state": "running", "pid": 4242, "uptime": 3723,
            "overlay": self.slot("slot1").overlay, "qmp_state": "running"})
        self.assertEqual(stopped["state"], "stopped")
        self.assertEqual(stopped["qmp_state"], "absent")
        self.assertEqual(report["counts"], {"running": 1, "stopped": 1, "stale": 0})

    def test_a_slot_with_no_leftovers_is_stopped(self):
        for entry in self.report()["slots"]:
            self.assertEqual(entry["state"], "stopped")
            self.assertIsNone(entry["pid"])
            self.assertIsNone(entry["uptime"])
            self.assertEqual(entry["qmp_state"], "absent")

    def test_a_dead_pid_with_a_leftover_socket_is_stale_not_running(self):
        with open(self.slot("slot1").qmp, "w") as f:
            f.write("leftover socket\n")
        self.record("slot1", 5150)
        stale = self.report()["slots"][0]
        self.assertEqual(stale["state"], "stale")
        self.assertIsNone(stale["pid"])
        self.assertIsNone(stale["uptime"])
        self.assertEqual(stale["qmp_state"], "unresponsive")

    def test_a_leftover_pidfile_alone_is_also_stale(self):
        self.record("slot2", 5150)
        stale = self.report()["slots"][1]
        self.assertEqual(stale["state"], "stale")
        self.assertEqual(stale["qmp_state"], "absent")

    def test_a_paused_guest_keeps_its_process_state_and_reports_pause(self):
        server = self.qmp_server("slot1")
        server.on("query-status", lambda c, a: {"status": "paused", "running": False})
        self.record("slot1", 4242)
        entry = self.report(Probe({4242}))["slots"][0]
        self.assertEqual(entry["state"], "running")
        self.assertEqual(entry["qmp_state"], "paused")

    def test_a_hung_socket_is_unresponsive_within_the_deadline(self):
        server = fake_qmp.FakeQMP()
        self.addCleanup(server.close)
        server.greeting = None  # accepts the connection, never answers
        os.symlink(server.path, self.slot("slot1").qmp)
        self.record("slot1", 4242)
        started = time.monotonic()
        entry = self.report(Probe({4242}), timeout=0.25)["slots"][0]
        self.assertLess(time.monotonic() - started, 1.5)
        self.assertEqual(entry["state"], "running")
        self.assertEqual(entry["qmp_state"], "unresponsive")

    def test_json_and_text_show_the_same_slot_fields(self):
        server = self.qmp_server("slot1")
        server.on("query-status", lambda c, a: {"status": "running"})
        self.record("slot1", os.getpid())
        rc, text = self.run_cli("status")
        self.assertEqual(rc, 0)
        rc, raw = self.run_cli("status", "--json")
        self.assertEqual(rc, 0)
        report = json.loads(raw)
        running, stopped = report["slots"]
        self.assertEqual(running["state"], "running")
        self.assertEqual(running["pid"], os.getpid())
        self.assertEqual(running["qmp_state"], "running")
        self.assertEqual(report["counts"], {"running": 1, "stopped": 1, "stale": 0})
        lines = text.splitlines()
        self.assertIn("slot1 state=running pid=%d uptime=%s qmp_state=running overlay=%s"
                      % (os.getpid(), provision.format_uptime(running["uptime"]),
                         running["overlay"]), lines)
        self.assertIn("slot2 state=stopped pid=- uptime=- qmp_state=absent overlay=%s"
                      % (stopped["overlay"],), lines)
        self.assertEqual(lines[-1], "summary running=1 stopped=1 stale=0")
