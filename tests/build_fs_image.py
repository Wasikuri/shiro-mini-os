#!/usr/bin/env python3
"""Build an SHFS disk image on the host with one file in it.

Layout (512-byte sectors): 0 superblock, 1..8 directory
(64 entries x 64 B: name[28], used, start_sector, size_bytes,
alloc_sectors, reserved[5]), 9..12 bitmap, 16.. data (contiguous).

Usage: build_fs_image.py <image> <name> <file-to-embed>
"""
import struct
import sys

SECT = 512
TOTAL = 16384  # 8 MB


def main():
    img_path, name, src_path = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(src_path, "rb") as f:
        data = f.read()
    img = bytearray(TOTAL * SECT)
    alloc = (len(data) + SECT - 1) // SECT
    start = 16
    # superblock
    struct.pack_into("<7I", img, 0, 0x53484653, TOTAL, 1, 8, 9, 4, 16)
    # directory entry 0 (at sector 1)
    off = 1 * SECT
    nb = name.encode()[:27]
    img[off:off + len(nb)] = nb
    struct.pack_into("<5I", img, off + 28, 1, start, len(data), alloc, 0)
    # bitmap: sectors 0..15 + file run
    bm_off = 9 * SECT
    for s in list(range(16)) + list(range(start, start + alloc)):
        img[bm_off + s // 8] |= (1 << (s % 8))
    # data
    img[start * SECT:start * SECT + len(data)] = data
    with open(img_path, "wb") as f:
        f.write(img)
    print(f"wrote {img_path}: {name} ({len(data)} bytes, {alloc} sectors)")


if __name__ == "__main__":
    main()
