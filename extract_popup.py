#!/usr/bin/env python3
"""
Emit the little number the game throws up when a blow scores.

It is not a banner and it does not say "BONUS": it is just the points, 100, 200, 400,
500, 800 or 1600, in four characters on the ground beside the fighters.

The mechanism is a character-set swap. $46F0 takes the announcement number $613F (0..5),
looks up an offset in $46EA and copies sixteen bytes from $467A into characters $30/$31
of BOTH lower character sets, plus sixteen fixed bytes from $46DA into $32/$33 -- so the
first two characters carry the changing digits and the last two are always "00":

    46F0: CMP #$06 / BCS ..        ; only six of them exist
    46F4: TAY / LDA $46EA,Y / TAY
    46F9: LDX #$0F
    46FB: LDA $467A,Y / STA $1180,X / STA $1580,X
    4704: LDA $46DA,X / STA $1190,X / STA $1590,X
    470D: DEY / DEX / BPL $46FB

Then $4660 puts those four characters on screen, at row 4 of the ground text region and
at the column $6140 the hit test worked out from the attacker's position:

    4660: LDA $6119 / STA $63 / LDA #$C0 / STA $62    ; $08C0 or $18C0 -- row 4
    4669: LDA #$B0 / LDY $6140 / LDX #$03
    4670: STA ($62),Y / ADC #$01 / INY / DEX / BPL

The codes are $B0..$B3 -- $30..$33 with bit 7 set, which in ANTIC mode 4 sends pixel
value 3 to COLPF3 instead of COLPF2. For this band COLPF3 is $0F and COLPF0 is $00, so
the digits are white with a black shadow, and that is what the captures show.

Usage: python3 extract_popup.py [dump.bin]
"""
import glob
import os
import sys

import numpy as np
from PIL import Image

INDEX = 0x46EA          # $613F -> an offset into $467A
VARIABLE = 0x467A       # the changing pair of characters, 16 bytes each
FIXED = 0x46DA          # the trailing "00", always the same 16 bytes
COUNT = 6
CHARS = 4
LINES = 8
PX_PER_CHAR = 4
CLOCKS_PER_PX = 2

# Measured off the screen, from a capture whose dump carries the four codes in the row:
# the field's top scanline and the left clock of column 0.
TOP = 161
LEFT = 32
COL_CLOCKS = PX_PER_CHAR * CLOCKS_PER_PX

WHITE = (211, 211, 211)     # COLPF3 = $0F, the digits
BLACK = (4, 4, 4)           # COLPF0 = $00, the shadow

OUT = "generated/popup.h"
DUMPS = ("extracted/ram_true_64k.bin", "extracted/colour4/bg_e1.bin",
         "extracted/fightdumps/dump_00.bin")
# the capture that caught one on screen, and where it sat
CHECK_PNG = "extracted/colour4/bg_e1.png"
CHECK_DUMP = "extracted/colour4/bg_e1.bin"


def find_dump():
    if len(sys.argv) > 1:
        return sys.argv[1]
    for p in DUMPS:
        if os.path.exists(p) and os.path.getsize(p) > 0x4700:
            return p
    return None


def glyphs(d):
    """The four 8-byte characters for each of the six announcements."""
    out = []
    for i in range(COUNT):
        y = d[INDEX + i]
        var = [0] * 16
        for x in range(15, -1, -1):          # $46FB counts both registers down
            var[x] = d[VARIABLE + y]
            y = (y - 1) & 0xFF
        fixed = [d[FIXED + x] for x in range(16)]
        out.append([var[0:8], var[8:16], fixed[0:8], fixed[8:16]])
    return out


def render(chars):
    """One announcement as a grid of ANTIC mode 4 pixel values."""
    g = np.zeros((LINES, CHARS * PX_PER_CHAR), np.uint8)
    for c, glyph in enumerate(chars):
        for line in range(LINES):
            b = glyph[line]
            for k in range(PX_PER_CHAR):
                g[line, c * PX_PER_CHAR + k] = (b >> (6 - 2 * k)) & 3
    return g


def self_check(all_glyphs):
    """Draw one back over the capture that caught it, and insist it matches.

    bg_e1 has the four codes sitting in its screen memory and its character set holding
    the "200" pattern, so the announcement showing there is number 1.
    """
    if not (os.path.exists(CHECK_PNG) and os.path.exists(CHECK_DUMP)):
        print("  no capture to check against -- skipped")
        return 0, 0
    im = np.array(Image.open(CHECK_PNG).convert("RGB")).astype(int)[::2, ::2]
    d = open(CHECK_DUMP, "rb").read()
    # which announcement: match the capture's own character set against the six
    chb = d[0x611F] << 8
    live = [[d[chb + c * 8 + line] for line in range(LINES)] for c in range(0x30, 0x34)]
    which = None
    for i, g in enumerate(all_glyphs):
        if g == live:
            which = i
            break
    if which is None:
        print("  the capture's character set is not one of the six -- skipped")
        return 0, 0
    grid = render(all_glyphs[which])
    # the column the popup sat at, found by where its ink lands
    best = None
    for col in range(4, 40):
        x0 = LEFT + col * COL_CLOCKS
        diff = tot = 0
        for y in range(LINES):
            for x in range(grid.shape[1]):
                v = grid[y, x]
                if v != 3 and v != 1:
                    continue
                want = WHITE if v == 3 else BLACK
                for c in range(CLOCKS_PER_PX):
                    sx = x0 + x * CLOCKS_PER_PX + c
                    if sx >= im.shape[1]:
                        continue
                    tot += 1
                    if tuple(im[TOP + y, sx]) != want:
                        diff += 1
        if best is None or diff < best[1]:
            best = (col, diff, tot)
    col, diff, tot = best
    print("  announcement %d found in %s at column %d: %d of %d ink pixels differ"
          % (which, os.path.basename(CHECK_PNG), col, diff, tot))
    return diff, tot


def main():
    path = find_dump()
    if path is None:
        # not in this repository -- see README.md
        print("no RAM dump holding $467A -- skipping")
        return 0
    d = open(path, "rb").read()
    all_glyphs = glyphs(d)

    print("%s: %d announcements, %d characters each" % (os.path.basename(path),
                                                        COUNT, CHARS))
    for i, chars in enumerate(all_glyphs):
        g = render(chars)
        print("  $613F = %d" % i)
        for line in range(6):
            print("     |" + "".join(" .oO"[v] for v in g[line]) + "|")

    diff, tot = self_check(all_glyphs)
    if tot and diff:
        print("refusing to emit")
        return 1

    out = []
    out.append("/* popup.h - the points a blow scores, thrown up on the ground.\n"
               " *\n"
               " * Not a banner and not the word BONUS: just the number, 100 to 1600, in\n"
               " * four characters. $46F0 swaps the digits into characters $30/$31 of the\n"
               " * lower character set by the announcement number $613F, $46DA supplies the\n"
               " * trailing \"00\" in $32/$33, and $4660 lays codes $B0..$B3 into row 4 of the\n"
               " * ground at the column $6140 the hit test computed.\n"
               " *\n"
               " * Bit 7 of those codes sends pixel value 3 to COLPF3, which is $0F for this\n"
               " * band; value 1 is COLPF0, $00. So: white digits, black shadow.\n"
               " * GENERATED FILE - do not edit; re-run extract_popup.py instead. */\n")
    out.append("#ifndef POPUP_H\n#define POPUP_H\n#include <stdint.h>\n")
    out.append("#define POPUP_COUNT %d\n#define POPUP_CHARS %d\n#define POPUP_LINES %d\n"
               % (COUNT, CHARS, LINES))
    out.append("#define POPUP_PX_PER_CHAR %d\n#define POPUP_CLOCKS_PER_PX %d\n"
               % (PX_PER_CHAR, CLOCKS_PER_PX))
    out.append("/* row 4 of the ground text region, measured on a capture that caught one */\n")
    out.append("#define POPUP_TOP %d\n#define POPUP_LEFT %d\n" % (TOP, LEFT))
    out.append("/* the colours the band's COLPF3 and COLPF0 put on screen */\n")
    out.append("#define POPUP_COL_INK %d,%d,%d\n" % WHITE)
    out.append("#define POPUP_COL_SHADOW %d,%d,%d\n" % BLACK)
    out.append("/* mode 4 pixel values, 0 and 2 transparent here */\n")
    out.append("static const uint8_t POPUP_PX[POPUP_COUNT]"
               "[POPUP_LINES][POPUP_CHARS*POPUP_PX_PER_CHAR]={\n")
    for chars in all_glyphs:
        g = render(chars)
        out.append("  {%s},\n" % ",".join(
            "{%s}" % ",".join(str(int(v)) for v in row) for row in g))
    out.append("};\n#endif\n")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").write("".join(out))
    print("wrote %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
