#!/usr/bin/env python3
"""Generate art.c / art.h for win: fonts, icons and mouse pointers.

    python3 mkart.py

The fonts come from the console's 6x8 font (font_cp437_6x8.c in the
kernel): a proportional version for the user interface (each glyph
trimmed to its own width) and the fixed 6x8 one for Notepad. The icons
and pointers are drawn below as text, one character per pixel.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_C = os.path.join(HERE, "../../linux-2.0.x/arch/m68knommu/platform/"
                      "68000/68Katy/font_cp437_6x8.c")

# icon colours: the 16 colours of the palette (see win.h), '.' = transparent
ICON_COLOURS = {
    'k': 0, 'm': 1, 'G': 2, 'o': 3, 'n': 4, 'p': 5, 't': 6, 's': 7,
    'g': 8, 'r': 9, 'l': 10, 'y': 11, 'b': 12, 'f': 13, 'c': 14, 'w': 15,
}

ICONS = {}       # 16x16

ICONS['group'] = """
................
.kkkkkkkkkkkkkk.
.knnnnnnnnnnnnk.
.kkkkkkkkkkkkkk.
.kwwwwwwwwwwwwk.
.kwrrwwbbwwyywk.
.kwrrwwbbwwyywk.
.kwwwwwwwwwwwwk.
.kwggwwggwwggwk.
.kwwwwwwwwwwwwk.
.kwllwwccwwffwk.
.kwllwwccwwffwk.
.kwwwwwwwwwwwwk.
.kwggwwggwwggwk.
.kwwwwwwwwwwwwk.
.kkkkkkkkkkkkkk.
"""

ICONS['progman'] = """
................
kkkkkkkkkkkk....
knnnnnnnnnnk....
kkkkkkkkkkkk....
kwwwkkkkkkkkkkkk
kwwwknnnnnnnnnnk
kwwwkkkkkkkkkkkk
kwwwkwwwwwwwwwwk
kwwwkwrrwwbbwwwk
kkkkkwrrwwbbwwwk
....kwwwwwwwwwwk
....kwyywwllwwwk
....kwyywwllwwwk
....kwwwwwwwwwwk
....kkkkkkkkkkkk
................
"""

ICONS['notepad'] = """
................
..k.k.k.k.k.k...
.kwkwkwkwkwkwk..
.kssssssssssskg.
.kwwwwwwwwwwwkg.
.kwkkkkkkkkwwkg.
.kwwwwwwwwwwwkg.
.kwkkkkkkkwwwkg.
.kwwwwwwwwwwwkg.
.kwkkkkkkkkkwkg.
.kwwwwwwwwwwwkg.
.kwkkkkkwwwwwkg.
.kwwwwwwwwwwwkg.
.kwwwwwwwwwwwkg.
.kkkkkkkkkkkkkg.
..gggggggggggg..
"""

ICONS['calc'] = """
..kkkkkkkkkkkk..
..kssssssssssk..
..kskkkkkkkkskg.
..kskwwwwwwkskg.
..kskwwwwkwkskg.
..kskkkkkkkkskg.
..kssssssssssskg
..kswwswwswwskg.
..kssssssssssskg
..kswwswwsrrskg.
..kssssssssssskg
..kswwswwswwskg.
..kssssssssssskg
..kswwswwswwskg.
..kkkkkkkkkkkkg.
...gggggggggggg.
"""

ICONS['clock'] = """
.....kkkkkk.....
...kkttttttkk...
..ktwwwwkwwwtk..
.ktwwwwwkwwwwtk.
.ktwwwwwkwwwwtk.
ktwwwwwwkwwwwwtk
ktkwwwwwkwwwwktk
ktwwwwwwkkkkwwtk
ktwwwwwwwwwwwwtk
ktwwwwwwwwwwwwtk
.ktwwwwwwwwwwtk.
.ktwwwwwwwwwwtk.
..ktwwwwkwwwtk..
...kkttttttkk...
.....kkkkkk.....
................
"""

ICONS['filemgr'] = """
................
................
..kkkkk.........
.kyyyyyk........
.kyyyyyykkkkkkk.
.kyyyyyyyyyyyyk.
.kyykkkkkkkkkkkk
.kyykwwwwwwwwwwk
.kykwkkkkkkkwwk.
.kykwwwwwwwwwwk.
.kkwkkkkkkkwwk..
.kkwwwwwwwwwwk..
.kkkkkkkkkkkkk..
................
................
................
"""

ICONS['paint'] = """
................
.....kkkkkk.....
...kksssssskk...
..ksrrssssbbsk..
.ksrrrsssbbbssk.
.ksssssssssssllk
ksyyssskkssslllk
ksyyysk..ksssssk
kssssk...kssssk.
ksssssk..kssk...
kscccssskkssk...
ksccsssssssk....
.kssGGsssssk....
..kssGGsskk.....
...kkkkkk.......
................
"""

ICONS['puzzle'] = """
kkkkkkkkkkkkkkkk
kwwwkwwwkwwwkwwk
kwrrkwbbkwyykwwk
kwrrkwbbkwyykwwk
kkkkkkkkkkkkkkkk
kwwwkwwwkwwwkwwk
kwllkwcckwffkwwk
kwllkwcckwffkwwk
kkkkkkkkkkkkkkkk
kwwwkwwwkwwwknnk
kwggkwmmkwppknnk
kwggkwmmkwppknnk
kkkkkkkkkkkkkkkk
................
................
................
"""

ICONS['readme'] = """
..kkkkkkkkk.....
..kwwwwwwwkk....
..kwwwwwwwkwk...
..kwkkkkwwkkkk..
..kwwwwwwwwwwk..
..kwkkkkkkkkwk..
..kwwwwwwwwwwk..
..kwkkkkkkkwwk..
..kwwwwwwwwwwk..
..kwkkkkkkkkwk..
..kwwwwwwwwwwk..
..kwkkkkkwwwwk..
..kwwwwwwwwwwk..
..kkkkkkkkkkkk..
................
................
"""

ICONS['control'] = """
................
.kkkkkkkkkkkkkk.
.kssssssssssssk.
.kskkkkkkkkkksk.
.kskccccccccksk.
.kskcccbbbccksk.
.kskccccccccksk.
.kskkkkkkkkkksk.
.kssssssssssssk.
.kkkkkkkkkkkkkk.
...ksswssswssk..
...kskkskkskk...
...kswsswsswsk..
...kkkkkkkkkkk..
................
................
"""

ICONS['cp_color'] = """
................
.kkkkkkkkkkkkkk.
.krrrrkyyyykllk.
.krrrrkyyyykllk.
.krrrrkyyyykllk.
.kkkkkkkkkkkkkk.
.kbbbbkffffkcck.
.kbbbbkffffkcck.
.kbbbbkffffkcck.
.kkkkkkkkkkkkkk.
.knnnnkmmmmkGGk.
.knnnnkmmmmkGGk.
.knnnnkmmmmkGGk.
.kkkkkkkkkkkkkk.
................
................
"""

ICONS['cp_mouse'] = """
.......kk.......
.........k......
.........k......
.....kkkkkkk....
....kwwwkwwwk...
...kwwwwkwwwwk..
...kwwwwkwwwwk..
...kkkkkkkkkkk..
...kwwwwwwwwwk..
...kwwwwwwwwwk..
...kwwwwwwwwsk..
...kwwwwwwwwsk..
....kwwwwwwsk...
....kswwwwssk...
.....kkkkkkk....
................
"""

ICONS['cp_date'] = """
................
.kkkkkkkkk......
.krrrrrrrk......
.kkkkkkkkk......
.kwkwkwkwk......
.kkkkkkkkk.kkk..
.kwkwkwkwkkwwwk.
.kkkkkkkkkwwkwwk
.kwkwkwkkwwwkwwk
.kkkkkkkkwwwkkkk
.kwkwkwkkwwwwwwk
.kkkkkkkkkwwwwwk
..........kwwwk.
...........kkk..
................
................
"""

ICONS['terminal'] = """
................
kkkkkkkkkkkkkkkk
knnnnnnnnnnnnnnk
kkkkkkkkkkkkkkkk
kkkkkkkkkkkkkkkk
kkwkkkkkkkkkkkkk
kkkwkkkkkkkkkkkk
kkkkwkkkkkkkkkkk
kkkwkkkkkkkkkkkk
kkwkkwwwkkkkkkkk
kkkkkkkkkkkkkkkk
kkkkkkkkkkkkkkkk
kkkkkkkkkkkkkkkk
kkkkkkkkkkkkkkkk
kkkkkkkkkkkkkkkk
................
"""

ICONS['music'] = """
................
......kkkkkkkkk.
......kkkkkkkkk.
......k.......k.
......k.......k.
......k.......k.
......k.......k.
......k.......k.
......k.......k.
......k.......k.
....kkk.....kkk.
...kkkk....kkkk.
...kkkk....kkkk.
....kk......kk..
................
................
"""

ICONS['mb_stop'] = """
.....kkkkkk.....
...kkrrrrrrkk...
..krrrrrrrrrrk..
.krrrwrrrrwrrrk.
.krrwwwrrwwwrrk.
krrrrwwwwwwrrrrk
krrrrrwwwwrrrrrk
krrrrrwwwwrrrrrk
krrrrwwwwwwrrrrk
krrrwwwrrwwwrrrk
.krrrwrrrrwrrrk.
.krrrrrrrrrrrrk.
..krrrrrrrrrrk..
...kkrrrrrrkk...
.....kkkkkk.....
................
"""

ICONS['mb_info'] = """
.....kkkkkk.....
...kkwwwwwwkk...
..kwwwwbbwwwwk..
.kwwwwwbbwwwwwk.
.kwwwwwwwwwwwwk.
kwwwwwbbbwwwwwwk
kwwwwwwbbwwwwwwk
kwwwwwwbbwwwwwwk
kwwwwwwbbwwwwwwk
kwwwwwwbbwwwwwwk
.kwwwwwbbwwwwwk.
.kwwwwbbbbwwwwk.
..kwwwwwwwwwwk..
...kkwwwwwwkk...
.....kkkkkk.....
................
"""

ICONS['mb_question'] = """
.....kkkkkk.....
...kkwwwwwwkk...
..kwwwbbbbwwwk..
.kwwwbbwwbbwwwk.
.kwwwbbwwbbwwwk.
kwwwwwwwwbbwwwwk
kwwwwwwwbbwwwwwk
kwwwwwwbbwwwwwwk
kwwwwwwbbwwwwwwk
kwwwwwwwwwwwwwwk
.kwwwwwbbwwwwwk.
.kwwwwwbbwwwwwk.
..kwwwwwwwwwwk..
...kkwwwwwwkk...
.....kkkkkk.....
................
"""

ICONS['mb_excl'] = """
.......kk.......
......kyyk......
......kyyk......
.....kyyyyk.....
.....kykkyk.....
....kyykkyyk....
....kyykkyyk....
...kyyykkyyyk...
...kyyykkyyyk...
..kyyyykkyyyyk..
..kyyyyyyyyyyk..
.kyyyyykkyyyyyk.
.kyyyyykkyyyyyk.
kyyyyyyyyyyyyyyk
kkkkkkkkkkkkkkkk
................
"""

SMALL = {}       # 8x8, for lists

SMALL['folder'] = """
.kkk....
kyyyk...
kyyyykkk
kyyyyyyk
kyyyyyyk
kyyyyyyk
kkkkkkkk
........
"""

SMALL['file'] = """
kkkkk...
kwwwkk..
kwwwwk..
kwkkwk..
kwwwwk..
kwkkwk..
kwwwwk..
kkkkkk..
"""

SMALL['program'] = """
kkkkkkkk
knnnnnnk
kkkkkkkk
kwwwwwwk
kwwwwwwk
kwwwwwwk
kkkkkkkk
........
"""

SMALL['up'] = """
...k....
..kyk...
.kyyyk..
kyyyyyk.
..kyk...
..kyk...
..kkk...
........
"""

SMALL['pencil'] = """
......kk
.....kyk
....kyk.
...kyk..
..kyk...
.kkk....
.kk.....
k.......
"""

SMALL['brush'] = """
.....kkk
....kkk.
...kkk..
....k...
...kk...
..kkk...
.kkkk...
kkk.....
"""

SMALL['line'] = """
k.......
.k......
..k.....
...k....
....k...
.....k..
......k.
.......k
"""

SMALL['rect'] = """
kkkkkkkk
k......k
k......k
k......k
k......k
k......k
kkkkkkkk
........
"""

SMALL['frect'] = """
kkkkkkkk
kggggggk
kggggggk
kggggggk
kggggggk
kggggggk
kkkkkkkk
........
"""

SMALL['fill'] = """
...k....
..kwk...
.kwwwk..
kwwwwwk.
.kwwwkbb
..kwk.bb
...k...b
........
"""

SMALL['eraser'] = """
...kkkkk
..kwwwkk
.kwwwkwk
kkkkkwk.
kwwwkk..
kkkkk...
........
........
"""

SMALL['picker'] = """
......kk
.....kkk
....kkk.
...kwk..
..kwk...
.kwk....
.kk.....
k.......
"""

# pointers: '1' black, '2' white, '.' transparent; (hot x, hot y);
# halo = surround with white
POINTERS = []

POINTERS.append(('arrow', 0, 0, False, """
1...........
11..........
121.........
1221........
12221.......
122221......
1222221.....
12222221....
122222221...
1222222221..
12222211111.
1221221.....
121.1221....
11..1221....
1....1221...
.....111....
"""))

POINTERS.append(('wait', 5, 7, False, """
11111111111
12222222221
.122222221.
.121111121.
..1211121..
...12121...
....121....
....121....
...12221...
..1222221..
.122212221.
.121111121.
12111111121
11111111111
"""))

POINTERS.append(('ibeam', 4, 7, True, """
.........
.111.111.
....1....
....1....
....1....
....1....
....1....
....1....
....1....
....1....
....1....
....1....
....1....
.111.111.
"""))

POINTERS.append(('size_ns', 5, 7, True, """
...........
.....1.....
....111....
...11111...
..1111111..
.....1.....
.....1.....
.....1.....
.....1.....
.....1.....
..1111111..
...11111...
....111....
.....1.....
"""))

POINTERS.append(('size_we', 7, 5, True, """
...............
...1.......1...
..11.......11..
.111.......111.
1111111111111111
.111.......111.
..11.......11..
...1.......1...
"""))

POINTERS.append(('size_nwse', 6, 6, True, """
.............
.11111.......
.1111........
.111.........
.11.1........
.1...1.......
......1......
.......1...1.
........1.11.
.........111.
........1111.
.......11111.
"""))

POINTERS.append(('size_nesw', 6, 6, True, """
.............
.......11111.
........1111.
.........111.
........1.11.
.......1...1.
......1......
.1...1.......
.11.1........
.111.........
.1111........
.11111.......
"""))

POINTERS.append(('cross', 7, 7, True, """
...............
.......1.......
.......1.......
.......1.......
.......1.......
.......1.......
...............
.11111...11111.
...............
.......1.......
.......1.......
.......1.......
.......1.......
.......1.......
...............
"""))


def rows(art, size):
    r = [line for line in art.strip('\n').split('\n')]
    for line in r:
        assert len(line) <= size, (line, len(line))
    r = [line.ljust(size, '.') for line in r]
    assert len(r) <= size, len(r)
    return r + ['.' * size] * (size - len(r))


def load_font():
    s = open(FONT_C).read()
    body = s[s.index('{'):]
    vals = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', body)]
    return [vals[i * 8:i * 8 + 8] for i in range(256)]


def c_bytes(vals, per_line=16, indent='\t'):
    out = []
    for i in range(0, len(vals), per_line):
        out.append(indent + ', '.join('0x%02X' % v for v in vals[i:i + per_line]) + ',')
    return '\n'.join(out)


def main():
    font = load_font()
    c = ['/* GENERATED by mkart.py - do not edit. */', '#include "art.h"', '']

    # proportional font: each glyph trimmed, bit 7 = leftmost pixel
    widths, bits = [], []
    for ch in range(32, 127):
        g = font[ch]
        used = 0
        for v in g:
            used |= v
        if ch == 32 or not used:
            widths.append(3)
            bits.append([0] * 8)
            continue
        left = max(b for b in range(6) if used & (1 << b))     # highest bit
        right = min(b for b in range(6) if used & (1 << b))
        w = left - right + 1
        widths.append(w + 1)                                    # + 1 gap
        bits.append([((v >> right) << (8 - w)) & 0xFF for v in g])
    c.append('const unsigned char font_width[95] = {')
    c.append(c_bytes(widths))
    c.append('};')
    c.append('const unsigned char font_bits[95 * 8] = {')
    c.append(c_bytes(sum(bits, [])))
    c.append('};')

    # fixed 6x8 font, bit 7 = leftmost pixel
    fixed = []
    for ch in range(32, 127):
        fixed += [(v << 2) & 0xFF for v in font[ch]]
    c.append('const unsigned char font_fixed[95 * 8] = {')
    c.append(c_bytes(fixed))
    c.append('};')
    c.append('')

    h = ['/* GENERATED by mkart.py - do not edit. */',
         '#ifndef ART_H', '#define ART_H', '',
         'extern const unsigned char font_width[95];',
         'extern const unsigned char font_bits[95 * 8];',
         'extern const unsigned char font_fixed[95 * 8];', '',
         '/* icons: one byte per pixel, colour 0 - 15 or 0xFF transparent */']

    for name, art in ICONS.items():
        px = [0xFF if ch == '.' else ICON_COLOURS[ch] for line in rows(art, 16) for ch in line]
        c.append('const unsigned char icon_%s[256] = {' % name)
        c.append(c_bytes(px))
        c.append('};')
        h.append('extern const unsigned char icon_%s[256];' % name)
    for name, art in SMALL.items():
        px = [0xFF if ch == '.' else ICON_COLOURS[ch] for line in rows(art, 8) for ch in line]
        c.append('const unsigned char small_%s[64] = {' % name)
        c.append(c_bytes(px))
        c.append('};')
        h.append('extern const unsigned char small_%s[64];' % name)

    # pointers: 16x16 sprites, tiles in sprite order (TL, BL, TR, BR)
    h.append('')
    h.append('/* mouse pointers: 4 sprite tiles each (palette 3: 1 black, 2 white) */')
    c.append('const unsigned int pointer_tiles[%d][32] = {' % len(POINTERS))
    hots = []
    for i, (name, hx, hy, halo, art) in enumerate(POINTERS):
        g = [list(line) for line in rows(art, 16)]
        if halo:
            for y in range(16):
                for x in range(16):
                    if g[y][x] == '.' and any(
                            0 <= y + dy < 16 and 0 <= x + dx < 16 and
                            g[y + dy][x + dx] == '1'
                            for dy in (-1, 0, 1) for dx in (-1, 0, 1)):
                        g[y][x] = 'h'
            g = [['2' if ch == 'h' else ch for ch in line] for line in g]
        longs = []
        for tx, ty in ((0, 0), (0, 1), (1, 0), (1, 1)):
            for r in range(8):
                v = 0
                for col in range(8):
                    ch = g[ty * 8 + r][tx * 8 + col]
                    v = (v << 4) | (0 if ch == '.' else int(ch))
                longs.append(v)
        c.append('\t{ /* %s */' % name)
        c.append('\n'.join('\t\t' + ', '.join('0x%08X' % v for v in longs[j:j + 8]) + ','
                           for j in range(0, 32, 8)))
        c.append('\t},')
        hots += [hx, hy]
        h.append('#define PTR_%s %d' % (name.upper(), i))
    c.append('};')
    c.append('const unsigned char pointer_hot[%d] = {' % (2 * len(POINTERS)))
    c.append(c_bytes(hots))
    c.append('};')
    h.append('#define PTR_COUNT %d' % len(POINTERS))
    h.append('extern const unsigned int pointer_tiles[PTR_COUNT][32];')
    h.append('extern const unsigned char pointer_hot[PTR_COUNT * 2];')
    h.append('')
    h.append('#endif')

    open(os.path.join(HERE, 'art.c'), 'w').write('\n'.join(c) + '\n')
    open(os.path.join(HERE, 'art.h'), 'w').write('\n'.join(h) + '\n')


if __name__ == '__main__':
    main()
