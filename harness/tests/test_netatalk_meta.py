"""Host tests for the moved netatalk sidecar/metadata layout; no share, guest or root."""
import contextlib
import io
import os
import struct
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
import netatalk_meta as meta  # noqa: E402


class BuildDir:
    """A local build tree with Rez output for one target."""

    def __init__(self, name, resource=b"RSRC"):
        self.tmp = tempfile.TemporaryDirectory()
        self.path = self.tmp.name
        self.name = name
        self.resource = resource
        os.makedirs(os.path.join(self.path, ".rsrc"))
        with open(os.path.join(self.path, ".rsrc", name + ".APPL"), "wb") as f:
            f.write(resource)

    def close(self):
        self.tmp.cleanup()


class SidecarTest(unittest.TestCase):
    def setUp(self):
        self.build = BuildDir("SherclawkWriteCheck", resource=b"resource-bytes" * 4)
        self.addCleanup(self.build.close)

    def test_sidecar_bytes_carry_the_exact_netatalk_layout(self):
        data = meta.sidecar_bytes(self.build.path, self.build.name)
        self.assertEqual(data[:4], bytes.fromhex("00051607"))
        self.assertEqual(struct.unpack(">I", data[4:8])[0], 0x00020000)
        self.assertEqual(data[8:24], b"Netatalk        ")
        self.assertEqual(struct.unpack(">H", data[24:26])[0], 2)
        parsed = meta.parse_sidecar(data)
        self.assertEqual(parsed["resource"], self.build.resource)
        self.assertEqual(parsed["finderinfo"], meta.make_finderinfo(custom=False))
        self.assertEqual(parsed["entries"],
                         [(meta.ID_FINDERI, 50, 32),
                          (2, 82, len(self.build.resource))])

    def test_custom_icon_builds_sherclawk_metadata_only(self):
        sherclawk = BuildDir("Sherclawk")
        self.addCleanup(sherclawk.close)
        custom = meta.sidecar_bytes(sherclawk.path, "Sherclawk")
        self.assertEqual(meta.parse_sidecar(custom)["finderinfo"],
                         meta.make_finderinfo(custom=True))
        self.assertTrue(meta.uses_custom_icon("Sherclawk"))
        self.assertFalse(meta.uses_custom_icon("SherclawkWriteCheck"))

    def test_sidecar_written_by_the_cli_matches_sidecar_bytes(self):
        out = os.path.join(self.build.path, "._out")
        with contextlib.redirect_stdout(io.StringIO()):
            meta.sidecar(self.build.path, self.build.name, out)
        with open(out, "rb") as f:
            self.assertEqual(f.read(), meta.sidecar_bytes(self.build.path, self.build.name))

    def test_parse_sidecar_rejects_a_foreign_filler(self):
        data = bytearray(meta.sidecar_bytes(self.build.path, self.build.name))
        data[8:24] = b"Mac OS X        "
        with self.assertRaises(ValueError):
            meta.parse_sidecar(bytes(data))

    def test_parse_sidecar_rejects_truncated_tables_and_forks(self):
        data = meta.sidecar_bytes(self.build.path, self.build.name)
        with self.assertRaises(ValueError):
            meta.parse_sidecar(data[:26])
        with self.assertRaises(ValueError):
            meta.parse_sidecar(data[:-1])
        just_header = bytearray(data)
        just_header[24:26] = struct.pack(">H", 0)
        with self.assertRaises(ValueError):
            meta.parse_sidecar(bytes(just_header))


class MetadataTest(unittest.TestCase):
    def test_meta_record_parses_its_own_layout(self):
        finf = meta.make_finderinfo(with_dates=True, custom=False)
        record = meta.meta_record(1234, 5678, finf)
        self.assertEqual(len(record), 402)
        parsed = meta.parse_metadata(record)
        self.assertEqual(parsed["finderinfo"], finf)
        self.assertEqual((parsed["dev"], parsed["ino"]), (1234, 5678))
        self.assertEqual(parsed["table"],
                         [(rid, off, ln) for rid, off, ln in meta.ENTRY_TABLE])

    def test_parse_metadata_rejects_zeroed_or_truncated_records(self):
        with self.assertRaises(ValueError):
            meta.parse_metadata(b"\x00" * 402)
        with self.assertRaises(ValueError):
            meta.parse_metadata(b"\x00" * 122)
        record = bytearray(meta.meta_record(1, 2, meta.make_finderinfo(with_dates=True)))
        record[374:382] = b"\x00" * 8
        with self.assertRaises(ValueError):
            meta.parse_metadata(bytes(record))


class StateTest(unittest.TestCase):
    def setUp(self):
        self.build = BuildDir("SherclawkWriteCheck", resource=b"rsrc" * 7)
        self.addCleanup(self.build.close)
        self.sidecar = meta.sidecar_bytes(self.build.path, self.build.name)
        self.finf = meta.make_finderinfo(with_dates=True, custom=False)
        self.metadata = meta.meta_record(9, 10, self.finf)

    def state(self, data=b"DATA", sidecar=None, metadata=None):
        return meta.state_of(self.build.name, data, sidecar, metadata)

    def test_state_accepts_a_complete_install(self):
        state = self.state(sidecar=self.sidecar, metadata=self.metadata)
        self.assertTrue(state["sidecar"]["finderinfo_matches"])
        self.assertTrue(state["metadata"]["finderinfo_matches"])
        self.assertEqual(meta.problems(state), [])
        self.assertEqual(state["sidecar"]["resource_sha256"],
                         meta.sha256(self.build.resource))

    def test_problems_reject_missing_sidecar_and_metadata(self):
        problems = meta.problems(self.state())
        self.assertTrue(any("sidecar" in p for p in problems), problems)
        self.assertTrue(any("metadata" in p for p in problems), problems)

    def test_problems_reject_an_unreadable_metadata_xattr(self):
        state = self.state(sidecar=self.sidecar)
        state["metadata_error"] = "xattrs unsupported"
        problems = meta.problems(state)
        self.assertTrue(any("netatalk metadata" in p for p in problems), problems)

    def test_problems_reject_data_and_resource_hash_mismatches(self):
        state = self.state(sidecar=self.sidecar, metadata=self.metadata)
        problems = meta.problems(state, expect_data_sha256="0" * 64,
                                 expect_resource_sha256="1" * 64)
        self.assertTrue(any("data fork" in p for p in problems), problems)
        self.assertTrue(any("resource fork" in p for p in problems), problems)

    def test_problems_reject_a_foreign_finder_record(self):
        foreign = meta.meta_record(9, 10, meta.make_finderinfo(with_dates=True, custom=True))
        state = self.state(sidecar=self.sidecar, metadata=foreign)
        problems = meta.problems(state)
        self.assertTrue(any("Finder info" in p for p in problems), problems)

    def test_inspect_path_reads_the_file_and_its_xattr(self):
        target = os.path.join(self.build.path, self.build.name)
        with open(target, "wb") as f:
            f.write(b"data-fork")
        sidecar_path = os.path.join(self.build.path, "._" + self.build.name)
        with open(sidecar_path, "wb") as f:
            f.write(self.sidecar)
        with mock.patch.object(meta, "read_metadata", return_value=self.metadata):
            state = meta.inspect_path(target)
        self.assertEqual(state["data_bytes"], 9)
        self.assertEqual(meta.problems(state), [])
        self.assertEqual(state["name"], self.build.name)


if __name__ == "__main__":
    unittest.main()
