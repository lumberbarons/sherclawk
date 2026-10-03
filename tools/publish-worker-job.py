#!/usr/bin/env python3
"""Publish a fresh data-fork job, closing every input before renaming READY.

This host diagnostic producer is not the Sherclawk tool interface. The native
producer will use the same protocol through the File Manager. Failed staging
is retained; IDs are never reused. Applications must be transported fork-aware,
not through this source-only helper.
"""
import argparse
import os
import re
from pathlib import Path


def publish(queue, job_id, script, inputs=()):
    if not re.fullmatch(r"[a-z0-9_-]{1,24}", job_id) or job_id == "worker-lock":
        raise ValueError("ID must be 1–24 lowercase ASCII letters, digits, _ or -")
    if not queue.is_dir() or queue.is_symlink():
        raise ValueError("queue must be a real directory")
    if not script.is_file() or script.is_symlink():
        raise ValueError("script must be a regular file")
    payload = {"script": script.read_bytes()}
    if len(payload["script"]) > 65536:
        raise ValueError("script must be a regular file of at most 64 KiB")
    if b"\r" in payload["script"] or b"\0" in payload["script"]:
        raise ValueError("script must be LF text")
    reserved = {"script", "ready", "ready.tmp", "claimed", "started", "started.tmp",
                "result", "result.tmp", "stdout", "stderr"}
    for path in inputs:
        name = path.name
        if (not path.is_file() or path.is_symlink() or ":" in name
                or len(name.encode("mac_roman")) > 31
                or name.casefold() in reserved or name.casefold() in {n.casefold() for n in payload}):
            raise ValueError(f"invalid or duplicate input: {name}")
        payload[name] = path.read_bytes()
    # Reserve permanently before writing anything; never remove failed stages.
    destination = queue / job_id
    destination.mkdir()
    for name, data in payload.items():
        with (destination / name).open("xb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
    with (destination / "ready.tmp").open("xb") as stream:
        stream.write(b"protocol=1\n")
        stream.flush()
        os.fsync(stream.fileno())
    (destination / "ready.tmp").rename(destination / "ready")
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("queue", type=Path)
    parser.add_argument("id")
    parser.add_argument("script", type=Path)
    parser.add_argument("--input", type=Path, action="append", default=[])
    args = parser.parse_args()
    print(publish(args.queue, args.id, args.script, args.input))


if __name__ == "__main__":
    main()
