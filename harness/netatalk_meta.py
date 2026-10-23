#!/usr/bin/env python3
"""
netatalk_meta.py — give files the metadata netatalk expects, so Mac OS 9
sees them as real Mac applications on the AFP share.

Background (learned the hard way against netatalk 3.1.18 on Ubuntu 24.04):

  * The resource fork lives in an AppleDouble v2 sidecar "._<name>".  Netatalk
    only accepts such a sidecar when the header's 16-byte filler field is
    exactly b"Netatalk        " — otherwise it is treated as a foreign
    "Mac OS X split fork", ignored, and even shows up as a visible file in
    the guest.
  * The Finder info (type/creator) lives in the "user.org.netatalk.Metadata"
    xattr.  It must be the *complete* 402-byte record: the 8-entry table plus
    the file's own device and inode in the private entries (little-endian).
    A zeroed or truncated record is silently ignored and the guest sees an
    untyped document ("could not be opened...").

Usage:
  netatalk_meta.py sidecar <build_dir> <AppName> <out_sidecar>
      host side: builds the ._ sidecar from Rez's .rsrc output

  netatalk_meta.py xattr <target_file>
      server side (as root): writes the Metadata xattr for target_file

  netatalk_meta.py check <target_file>
      server side: prints a JSON state of both forks and the xattr; exits 1
      when the layout is incomplete or malformed

`publish.py` imports this module for the sidecar bytes and for the verify
step; the CLI stays usable stand-alone.
"""
import hashlib
import json
import os
import struct
import sys

FILLER = b"Netatalk        "
XATTR_NAME = "user.org.netatalk.Metadata"

ID_COMMENT = 4
ID_FILEDATES = 8
ID_FINDERI = 9
ID_AFPFILEI = 14
ID_PRIVDEV = 0x80444556  # "\x80DEV"
ID_PRIVINO = 0x80494E4F  # "\x80INO"
ID_PRIVSYN = 0x8053594E  # "\x80SYN"
ID_PRIVID = 0x8053567E   # "\x80SV~"

ENTRY_TABLE = [
    (ID_COMMENT,   154, 200),
    (ID_FILEDATES, 354,  16),
    (ID_FINDERI,   122,  32),
    (ID_AFPFILEI,  370,   4),
    (ID_PRIVDEV,   374,   8),
    (ID_PRIVINO,   382,   8),
    (ID_PRIVSYN,   390,   8),
    (ID_PRIVID,    398,   4),
]

CUSTOM_ICON_APPS = ("Sherclawk", "SherclawkScrollCheck")


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def uses_custom_icon(app):
    """Only Sherclawk's own files carry its creator and bundle icon."""
    return app in CUSTOM_ICON_APPS


def make_finderinfo(with_dates=False, custom=True):
    """Keep the exact Finder record layout; identify the lobster-icon app."""
    tail = (bytes.fromhex("000000006abecef40000000000000000") if with_dates
            else b"\x00" * 16)
    fi = b"APPLShCk" if custom else b"APPL????"
    fi += b"\x24\x00" if custom else b"\x01\x00"                          # flags: kHasBundle | kHasCustomIcon
    fi += (31).to_bytes(2, "big") + (83).to_bytes(2, "big")   # icon position
    fi += b"\x00\x00"                          # folder id
    fi += tail
    assert len(fi) == 32, len(fi)
    return fi


def meta_record(dev, ino, finf):
    out = struct.pack(">II", 0x00051607, 0x00020000) + FILLER
    out += struct.pack(">H", len(ENTRY_TABLE))
    for rid, off, ln in ENTRY_TABLE:
        out += struct.pack(">III", rid, off, ln)
    assert len(out) == 122
    out += finf                                          # 122..154
    out += b"\x00" * 200                                 # 154..354 comment
    out += struct.pack(">IIII", 0x3251C3B4, 0x3251C3B4, 0x80000000, 0x3251C3B4)
    out += struct.pack(">I", 0)                          # AFPFILEI
    out += struct.pack("<Q", dev)                        # PRIVDEV
    out += struct.pack("<Q", ino)                        # PRIVINO
    out += bytes.fromhex("f3d2be6a00000000")             # PRIVSYN
    out += struct.pack("<I", 24)                         # PRIVID
    assert len(out) == 402, len(out)
    return out


def assemble_sidecar(finf, rsrc):
    off_fi = 26 + 12 * 2
    off_rs = off_fi + len(finf)
    out = struct.pack(">II", 0x00051607, 0x00020000) + FILLER + struct.pack(">H", 2)
    out += struct.pack(">III", ID_FINDERI, off_fi, len(finf))
    out += struct.pack(">III", 2, off_rs, len(rsrc))
    return out + finf + rsrc


def sidecar_bytes(build_dir, app):
    """The exact ._ sidecar for Rez's .rsrc output of one built application."""
    finf = make_finderinfo(with_dates=False, custom=uses_custom_icon(app))
    with open(os.path.join(build_dir, ".rsrc", app + ".APPL"), "rb") as f:
        rsrc = f.read()
    return assemble_sidecar(finf, rsrc)


def sidecar(build_dir, app, out_path):
    data = sidecar_bytes(build_dir, app)
    with open(out_path, "wb") as f:
        f.write(data)
    print(f"sidecar: {out_path} ({len(data)} bytes, rsrc {len(data) - 82})")


def xattr(target):
    st = os.lstat(target)
    rec = meta_record(st.st_dev, st.st_ino, make_finderinfo(with_dates=True, custom=uses_custom_icon(os.path.basename(target))))
    os.setxattr(target, XATTR_NAME, rec)
    print(f"xattr: {target} (dev={st.st_dev} ino={st.st_ino}, {len(rec)} bytes)")


def parse_sidecar(data):
    """Strict AppleDouble v2 reader for a netatalk resource-fork sidecar."""
    if len(data) < 38 or struct.unpack(">II", data[:8]) != (0x00051607, 0x00020000):
        raise ValueError("not an AppleDouble v2 sidecar")
    if data[8:24] != FILLER:
        raise ValueError("not a netatalk sidecar (filler field is not Netatalk)")
    count = struct.unpack(">H", data[24:26])[0]
    if not count or 26 + 12 * count > len(data):
        raise ValueError("truncated sidecar entry table")
    entries = []
    forks = {}
    for index in range(count):
        rid, off, ln = struct.unpack(">III", data[26 + 12 * index:38 + 12 * index])
        if off < 26 + 12 * count or off + ln > len(data):
            raise ValueError("sidecar entry out of bounds")
        entries.append((rid, off, ln))
        forks[rid] = data[off:off + ln]
    if len(forks.get(ID_FINDERI, b"")) != 32:
        raise ValueError("sidecar has no 32-byte Finder info")
    if 2 not in forks:
        raise ValueError("sidecar has no resource fork entry")
    return {"entries": entries, "finderinfo": forks[ID_FINDERI],
            "resource": forks[2]}


def parse_metadata(record):
    """Strict reader for the complete 402-byte netatalk Metadata record."""
    if len(record) != 402:
        raise ValueError("metadata record is %d bytes, not 402" % len(record))
    if struct.unpack(">II", record[:8]) != (0x00051607, 0x00020000) or record[8:24] != FILLER:
        raise ValueError("not a netatalk metadata record")
    if struct.unpack(">H", record[24:26])[0] != len(ENTRY_TABLE):
        raise ValueError("metadata record has a foreign entry table")
    table = []
    for index in range(len(ENTRY_TABLE)):
        rid, off, ln = struct.unpack(">III", record[26 + 12 * index:38 + 12 * index])
        table.append((rid, off, ln))
    if table != ENTRY_TABLE:
        raise ValueError("metadata record entry table is incomplete")
    finf = record[122:154]
    dev = int.from_bytes(record[374:382], "little")
    ino = int.from_bytes(record[382:390], "little")
    if not dev or not ino:
        raise ValueError("metadata record has no file identity (dev/ino are zero)")
    return {"table": table, "finderinfo": finf, "dev": dev, "ino": ino}


def read_metadata(path):
    """The raw netatalk Metadata xattr of a server-side file."""
    getxattr = getattr(os, "getxattr", None)
    if getxattr is None:
        raise OSError("this host's Python cannot read xattrs")
    return getxattr(path, XATTR_NAME)


def state_of(name, data, sidecar_data, metadata):
    """The verifiable fork/xattr state of one publication target.

    Pure: the caller supplies the data fork, the ._ sidecar bytes and the
    Metadata record (or None when absent). Suitable for a remote check step.
    """
    state = {
        "name": name,
        "data_bytes": len(data),
        "data_sha256": sha256(data),
        "sidecar": None,
        "metadata": None,
        "metadata_error": None,
    }
    if sidecar_data is not None:
        state["sidecar"] = {"bytes": len(sidecar_data), "sha256": sha256(sidecar_data)}
        try:
            parsed = parse_sidecar(sidecar_data)
        except ValueError as exc:
            state["sidecar"]["error"] = str(exc)
        else:
            state["sidecar"].update({
                "resource_bytes": len(parsed["resource"]),
                "resource_sha256": sha256(parsed["resource"]),
                "finderinfo": parsed["finderinfo"].hex(),
                "finderinfo_matches": parsed["finderinfo"] == make_finderinfo(
                    with_dates=False, custom=uses_custom_icon(name)),
            })
    if metadata is not None:
        state["metadata"] = {"bytes": len(metadata)}
        try:
            parsed = parse_metadata(metadata)
        except ValueError as exc:
            state["metadata"]["error"] = str(exc)
        else:
            state["metadata"].update({
                "finderinfo": parsed["finderinfo"].hex(),
                "finderinfo_matches": parsed["finderinfo"] == make_finderinfo(
                    with_dates=True, custom=uses_custom_icon(name)),
                "dev": parsed["dev"],
                "ino": parsed["ino"],
            })
    return state


def problems(state, expect_data_sha256=None, expect_resource_sha256=None):
    """Everything that makes this state unfit to publish, as plain strings."""
    found = []
    if expect_data_sha256 and state["data_sha256"] != expect_data_sha256:
        found.append("data fork does not match the built artifact")
    sidecar = state.get("sidecar")
    if sidecar is None:
        found.append("no resource-fork sidecar (._%s)" % state.get("name"))
    elif sidecar.get("error"):
        found.append("resource fork sidecar: %s" % sidecar["error"])
    else:
        if expect_resource_sha256 and sidecar.get("resource_sha256") != expect_resource_sha256:
            found.append("resource fork does not match the built artifact")
        if not sidecar.get("finderinfo_matches"):
            found.append("sidecar Finder info does not match the target")
    if state.get("metadata_error"):
        found.append("netatalk metadata unavailable: %s" % state["metadata_error"])
    elif state.get("metadata") is None:
        found.append("no netatalk metadata xattr (Finder type/creator missing)")
    else:
        if state["metadata"].get("error"):
            found.append("netatalk metadata: %s" % state["metadata"]["error"])
        elif not state["metadata"].get("finderinfo_matches"):
            found.append("netatalk metadata Finder info does not match the target")
    return found


def inspect_path(path):
    """Read and parse the target, its sidecar and its xattr on this host."""
    with open(path, "rb") as f:
        data = f.read()
    sidecar_path = os.path.join(os.path.dirname(path), "._" + os.path.basename(path))
    try:
        with open(sidecar_path, "rb") as f:
            sidecar_data = f.read()
    except OSError:
        sidecar_data = None
    metadata_error = None
    try:
        metadata = read_metadata(path)
    except OSError as exc:
        metadata = None
        metadata_error = str(exc)
    state = state_of(os.path.basename(path), data, sidecar_data, metadata)
    state["path"] = path
    state["metadata_error"] = metadata_error
    return state


def main():
    cmd = sys.argv[1]
    if cmd == "sidecar":
        sidecar(sys.argv[2], sys.argv[3], sys.argv[4])
    elif cmd == "xattr":
        xattr(sys.argv[2])
    elif cmd == "check":
        state = inspect_path(sys.argv[2])
        found = problems(state)
        print(json.dumps({"state": state, "problems": found}, indent=2, sort_keys=True))
        return 1 if found else 0
    else:
        sys.exit(__doc__)
    return 0


if __name__ == "__main__":
    sys.exit(main())
