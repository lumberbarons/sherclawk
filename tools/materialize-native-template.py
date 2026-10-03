#!/usr/bin/env python3
"""Materialize the native template as MacRoman/CR source and LF shell script.

Git keeps readable LF sources. MPW requires CR source in the guest, while
MacRelix sh requires LF; build-native.sh sets Finder TEXT through the guest.
Only a new destination is accepted, keeping prior projects and forks intact.
"""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("destination", type=Path)
args = parser.parse_args()
source = Path(__file__).resolve().parents[1] / "templates" / "ppc-toolbox"
files = {}
for name in ("main.c", "app.r", "build-native.sh", "capture-toolchain.sh"):
    value = (source / name).read_text(encoding="utf-8")
    if name.endswith((".c", ".r")):
        value = value.replace("\n", "\r")
    files[name] = value.encode("mac_roman")
args.destination.mkdir(parents=True, exist_ok=False)
for name, value in files.items():
    (args.destination / name).write_bytes(value)
print(args.destination)
