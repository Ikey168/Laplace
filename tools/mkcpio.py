#!/usr/bin/env python3
"""Pack a directory into a cpio "newc" archive: the machine's seed image (#247).

Deterministic: entries sorted by path, inode numbers in that order, owner 0,
and every time set to 0 (or to --mtime), so the same tree gives the same
bytes, and the recording, which pins the seed by CRC, stays valid across
rebuilds.

    tools/mkcpio.py rootfs build/rootfs.cpio [--add SRC:DEST ...]

--add puts a host file at DEST inside the image (e.g. a program built
elsewhere: --add build/lua:/bin/lua).
"""

import argparse
import os
import stat
import sys


def header(ino, mode, nlink, mtime, size, namesize):
    fields = [ino, mode, 0, 0, nlink, mtime, size, 0, 0, 0, 0, namesize, 0]
    return b"070701" + b"".join(b"%08X" % f for f in fields)


def pad4(n):
    return (4 - n % 4) % 4


def entry(out, ino, name, mode, data, mtime):
    nm = name.encode() + b"\0"
    h = header(ino, mode, 2 if stat.S_ISDIR(mode) else 1, mtime, len(data), len(nm))
    out.append(h + nm + b"\0" * pad4(len(h) + len(nm)))
    out.append(data + b"\0" * pad4(len(data)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("root")
    ap.add_argument("out")
    ap.add_argument("--add", action="append", default=[], help="SRC:DEST")
    ap.add_argument("--mtime", type=int, default=0)
    a = ap.parse_args()

    items = {}  # path in image -> (mode, data)
    if os.path.isdir(a.root):
        for dirpath, dirnames, filenames in os.walk(a.root):
            dirnames.sort()
            rel = os.path.relpath(dirpath, a.root)
            if rel != ".":
                items[rel] = (stat.S_IFDIR | 0o755, b"")
            for f in sorted(filenames):
                p = os.path.join(dirpath, f)
                r = os.path.normpath(os.path.join(rel, f))
                st = os.lstat(p)
                if stat.S_ISLNK(st.st_mode):
                    items[r] = (stat.S_IFLNK | 0o777, os.readlink(p).encode())
                elif stat.S_ISREG(st.st_mode):
                    if f == ".keep":
                        continue  # placeholder that makes git keep an empty directory
                    perm = 0o755 if st.st_mode & 0o111 else 0o644
                    with open(p, "rb") as fh:
                        items[r] = (stat.S_IFREG | perm, fh.read())
    for spec in a.add:
        src, dest = spec.split(":", 1)
        dest = dest.lstrip("/")
        parts = dest.split("/")
        for i in range(1, len(parts)):
            items.setdefault("/".join(parts[:i]), (stat.S_IFDIR | 0o755, b""))
        with open(src, "rb") as fh:
            items[dest] = (stat.S_IFREG | 0o755, fh.read())

    out = []
    for ino, name in enumerate(sorted(items), start=1):
        mode, data = items[name]
        entry(out, ino, name, mode, data, a.mtime)
    entry(out, 0, "TRAILER!!!", 0, b"", 0)
    blob = b"".join(out)
    blob += b"\0" * pad4(len(blob))
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    with open(a.out, "wb") as fh:
        fh.write(blob)
    print(f"{a.out}: {len(items)} entries, {len(blob)} bytes")


if __name__ == "__main__":
    sys.exit(main())
