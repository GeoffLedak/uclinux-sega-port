#!/usr/bin/env python3
"""Compress a .vgm file into a .vgz that vgmplay can show the title of.

    python3 mkvgz.py song.vgm music/song.vgz

The result is an ordinary gzip file, so other VGM players read it as
usual.  The song's title, game and composer are copied from its GD3 tag
(which sits at the end of the file) into the gzip header's comment field,
one per line.  vgmplay reads that comment straight away, instead of
unpacking the whole song on the 68000 to reach the tag.
"""
import struct
import sys
import zlib


def gd3_lines(vgm):
    """Title, game and composer from the GD3 tag, as Latin-1 text."""
    if len(vgm) < 0x18 or vgm[:4] != b"Vgm ":
        sys.exit("not a VGM file")
    rel = struct.unpack_from("<I", vgm, 0x14)[0]
    if not rel:
        return []
    off = 0x14 + rel
    if vgm[off:off + 4] != b"Gd3 ":
        return []
    size = struct.unpack_from("<I", vgm, off + 8)[0]
    # strings: title, title (jp), game, game (jp), system, system (jp),
    # author, author (jp), date, ripper, notes
    strings = vgm[off + 12:off + 12 + size].decode("utf-16-le").split("\0")
    lines = []
    for i in (0, 2, 6):
        if i < len(strings) and strings[i].strip():
            # vgmplay prints plain ASCII; anything else shows as '?'
            lines.append(strings[i].strip().encode("latin-1", "replace"))
    return lines


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__.strip().splitlines()[2].strip())
    vgm = open(sys.argv[1], "rb").read()
    comment = b"\n".join(gd3_lines(vgm)).replace(b"\0", b"")[:255]

    header = struct.pack("<BBBBIBB", 0x1F, 0x8B, 8,
                         0x10 if comment else 0,    # FCOMMENT
                         0, 2, 3)   # no time stamp, best compression, Unix
    if comment:
        header += comment + b"\0"
    z = zlib.compressobj(9, zlib.DEFLATED, -15)     # raw deflate data
    body = z.compress(vgm) + z.flush()
    trailer = struct.pack("<II", zlib.crc32(vgm), len(vgm) & 0xFFFFFFFF)
    open(sys.argv[2], "wb").write(header + body + trailer)
    print("%s: %d bytes -> %d" % (sys.argv[2], len(vgm),
                                  len(header) + len(body) + len(trailer)))
    for line in comment.split(b"\n"):
        if line:
            print("  " + line.decode("latin-1"))


if __name__ == "__main__":
    main()
