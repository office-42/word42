#!/usr/bin/env python3
# make-ico.py - pack the PNG icons into one Windows .ico, for the installer.
#
# Copyright (C) 2026 Andreas Røsdal
# SPDX-License-Identifier: GPL-3.0-or-later
#
#     python3 build-aux/make-ico.py data/icons build-aux/word42.ico
#
# An .ico may hold PNG-compressed images since Vista, so no image library
# is needed: the header lists each PNG with its size and offset.
import os, struct, sys

SIZES = (16, 24, 32, 48, 64, 128, 256)

def main(icons_dir, out_path):
    entries = []
    for size in SIZES:
        path = os.path.join(icons_dir, f"{size}x{size}", "apps", "org.word42.word42.png")
        if os.path.exists(path):
            with open(path, "rb") as f:
                entries.append((size, f.read()))
    if not entries:
        sys.exit("no PNG icons found under " + icons_dir)

    header = struct.pack("<HHH", 0, 1, len(entries))
    offset = len(header) + 16 * len(entries)
    directory = b""
    for size, data in entries:
        dim = 0 if size >= 256 else size       # 0 means 256
        directory += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    with open(out_path, "wb") as f:
        f.write(header + directory + b"".join(d for _, d in entries))
    print(f"{out_path}: {len(entries)} images")

if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "data/icons",
         sys.argv[2] if len(sys.argv) > 2 else "build-aux/word42.ico")
