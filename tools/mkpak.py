#!/usr/bin/env python3
"""
mkpak.py -- packs the Wolfenstein 3D shareware data (v1.4, *.WL1) into wolf3d.pak, the one asset the door sends.

    python3 tools/mkpak.py data door/wolf3d.pak

Format (read by module/src/wolftrace.c), all little-endian:
    "W3DPAK1\\0", u32 count, count x { char name[24]; u32 offset; u32 size }, then the files one after another.

Only the eight data files go in, unchanged, as the shareware terms ask. Names are lower case (Wolf4SDL opens them
that way) and sorted, so the same data always makes the same pack, and so the same hash: players who already have it
aren't sent it again.
"""
import os
import struct
import sys

DATA = ['audiohed', 'audiot', 'gamemaps', 'maphead', 'vgadict', 'vgagraph', 'vgahead', 'vswap']

def main():
    src, out = sys.argv[1], sys.argv[2]
    found = {n.lower(): n for n in os.listdir(src)}
    names = sorted(f'{d}.wl1' for d in DATA)
    blobs = []
    for n in names:
        if n not in found:
            sys.exit(f'{n}: missing from {src}')
        with open(os.path.join(src, found[n]), 'rb') as f:
            blobs.append(f.read())
    offset = 12 + 32 * len(names)
    table = b''
    for n, b in zip(names, blobs):
        table += struct.pack('<24sII', n.encode(), offset, len(b))
        offset += len(b)
    with open(out, 'wb') as f:
        f.write(b'W3DPAK1\0' + struct.pack('<I', len(names)) + table + b''.join(blobs))
    print(f'{out}: {len(names)} files, {offset} bytes')

if __name__ == '__main__':
    main()
