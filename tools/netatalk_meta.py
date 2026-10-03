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
"""
import os
import struct
import sys

FILLER = b"Netatalk        "

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


def sidecar(build_dir, app, out_path):
    finf = make_finderinfo(with_dates=False, custom=app in ("Sherclawk", "SherclawkScrollCheck"))
    rsrc = open(os.path.join(build_dir, ".rsrc", f"{app}.APPL"), "rb").read()
    off_fi = 26 + 12 * 2
    off_rs = off_fi + len(finf)
    sc = struct.pack(">II", 0x00051607, 0x00020000) + FILLER + struct.pack(">H", 2)
    sc += struct.pack(">III", ID_FINDERI, off_fi, len(finf))
    sc += struct.pack(">III", 2, off_rs, len(rsrc))
    sc += finf + rsrc
    with open(out_path, "wb") as f:
        f.write(sc)
    print(f"sidecar: {out_path} ({len(sc)} bytes, rsrc {len(rsrc)})")


def xattr(target):
    st = os.lstat(target)
    rec = meta_record(st.st_dev, st.st_ino, make_finderinfo(with_dates=True, custom=os.path.basename(target) in ("Sherclawk", "SherclawkScrollCheck")))
    os.setxattr(target, "user.org.netatalk.Metadata", rec)
    print(f"xattr: {target} (dev={st.st_dev} ino={st.st_ino}, {len(rec)} bytes)")


def main():
    cmd = sys.argv[1]
    if cmd == "sidecar":
        sidecar(sys.argv[2], sys.argv[3], sys.argv[4])
    elif cmd == "xattr":
        xattr(sys.argv[2])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
