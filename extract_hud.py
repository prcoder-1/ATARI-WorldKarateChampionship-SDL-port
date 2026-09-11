#!/usr/bin/env python3
"""
Emit the game's HUD as C data: its character set, its colours and its layout.

The HUD is the two ANTIC mode 4 rows the display list places above the playfield
($6262/$6265, LMS $61C0), 40 characters wide, eight scanlines each, starting at
scanline 13. Mode 4 is four colours per character -- two bits per pixel, four pixels
wide -- with the pixel values selecting COLBK, COLPF0, COLPF1 and then COLPF2 or COLPF3
according to bit 7 of the character code.

The character set lives at $6400 (CHBASE = $64). It carries several copies of the same
glyphs drawn in different pixel values, which is how the original gets differently
coloured text out of one playfield: $00-$09 are digits in value 3, $2C-$35 are taller
digits in value 2, $28-$2B are D/E/M/O in value 1, and so on.

Colours are recovered the same way the playfield's were (see invert_palette.py): the
pixel values are known from the charset and the screen bytes, the colours are read off a
full-frame capture, and re-rendering through the result must reproduce the capture.

Usage: python3 extract_hud.py [dump.bin] [capture.png]
"""
import collections
import glob
import os
import sys

from PIL import Image

CHBASE = 0x6400
NCHARS = 0x80
SCREEN = 0x61C0
COLS = 40
ROWS = 2
ROW_LINES = 8
HUD_TOP = 13                # scanline of the first HUD row, from the display list
LEFT = 32                   # playfield origin in colour clocks
CLOCKS_PER_PX = 2           # a mode 4 pixel is two colour clocks
OUT = "generated/hud.h"

# the layout, read out of the HUD update routine at $5BB2..$5C8B
LAYOUT = {
    "P1_LABEL_COL": 0x0A,   # $5BA4[0]: "1UP"
    "P2_LABEL_COL": 0x1B,   # $5BA4[1]: "2UP"
    "LABEL_COL": 14,        # $5BEE / $5C4E: "TIME" while playing, "DEMO" otherwise
    "TIMER_COL": 19,        # $61D3/$61D4: two big digits, BCD, leading zero blanked
    "RIGHT_COL": 23,        # $5C2A: "L" plus the level, or the win markers
    "BELT_COL": 14,         # $5FBE, row 1
    "P1_SCORE_END": 5,      # right-aligned score, normal colour
    "P2_SCORE_END": 39,     # right-aligned score, COLPF3 (bit 7 set)
}
# character columns the ippon markers cover; they are Players, not playfield
PM_COLS = {7, 8, 31, 32}
PIP_DATA = 0x324C
PIP_ROWS = 0x1F
BELT_TEXT = 0x5EFF          # $5FB8 reads from here and subtracts $36
BELT_OFFSETS = 0x5F53
BELT_THRESHOLD = 0x5F59     # $5F95 compares the score against these, in order
BELT_COLOUR = 0x5F60        # $5FA5; zero means the name flashes off $14
NBELTS = 6
BELT_LEN = 12

CHARS = {
    "SPACE": 0x0A, "DIGIT0": 0x00, "BIGDIGIT0": 0x2C, "LETTER_A": 0x0B,
    "U": 0x26, "P": 0x27, "L": 0x36, "WIN_EMPTY": 0x39, "WIN_P1": 0x3A,
    "WIN_P2": 0xBA, "COLOUR_ALT": 0x80,
}


def find(patterns):
    for p in patterns:
        got = sorted(glob.glob(p))
        if got:
            return got[0]
    return None


def frame(path):
    im = Image.open(path).convert("RGB")
    w, h = im.size
    return im.resize((w // 2, h // 2), Image.NEAREST).load()


def pixel_values(d):
    """(value, altcolour) for every pixel of the two HUD rows."""
    out = []
    for row in range(ROWS):
        for line in range(ROW_LINES):
            px = []
            for col in range(COLS):
                c = d[SCREEN + row * COLS + col]
                b = d[CHBASE + (c & 0x7F) * 8 + line]
                alt = 1 if c & 0x80 else 0
                for k in range(4):
                    px.append((((b >> (6 - 2 * k)) & 3), alt))
            out.append(px)
    return out


def recover(values, px):
    """Per scanline: value 0/1/2 -> a colour, value 3 -> one colour per alt flag."""
    table = []
    for line, row in enumerate(values):
        y = HUD_TOP + line
        votes = collections.defaultdict(collections.Counter)
        for i, (v, alt) in enumerate(row):
            key = (v, alt if v == 3 else 0)
            votes[key][px[LEFT + i * CLOCKS_PER_PX, y]] += 1
        entry = {}
        for key, cnt in votes.items():
            (col, _), = cnt.most_common(1)
            entry[key] = col
        table.append(entry)
    return table


def colour_at(entry, v, alt):
    return entry.get((v, alt if v == 3 else 0))


def main():
    dump = sys.argv[1] if len(sys.argv) > 1 else find(["extracted/scenes/*.bin"])
    shot = sys.argv[2] if len(sys.argv) > 2 else find(["extracted/scenes/*.png"])
    if dump is None or shot is None:
        # not in this repository -- see README.md for what to put where
        print("no scene dump and capture to work from -- skipping")
        return 0
    d = open(dump, "rb").read()
    px = frame(shot)
    print("%s + %s" % (os.path.basename(dump), os.path.basename(shot)))

    values = pixel_values(d)
    table = recover(values, px)

    # The ippon markers are Player/Missile objects drawn OVER the HUD ($31E1 builds
    # them from $324C), so those columns are not playfield and are excluded here.
    bad, overlay = 0, set()
    for line, row in enumerate(values):
        y = HUD_TOP + line
        for i, (v, alt) in enumerate(row):
            if colour_at(table[line], v, alt) != px[LEFT + i * CLOCKS_PER_PX, y]:
                if i // 4 in PM_COLS:
                    overlay.add(i // 4)
                else:
                    bad += 1
    total = len(values) * COLS * 4
    print("re-render vs capture: %d/%d playfield pixels differ "
          "(P/M markers over columns %s excluded)"
          % (bad, total, sorted(overlay) or "none"))
    if bad:
        print("NOT emitting: the colour model does not reproduce the capture")
        return 1

    pal, idx = [], {}

    def ci(c):
        if c not in idx:
            idx[c] = len(pal)
            pal.append(c)
        return idx[c]

    def nearest(line, v, alt):
        """A blank scanline shows only value 0, so borrow the missing colours from the
        closest line that does use them."""
        for step in range(len(table)):
            for l in (line - step, line + step):
                if 0 <= l < len(table):
                    c = colour_at(table[l], v, alt)
                    if c is not None:
                        return c
        return (0, 0, 0)

    # per scanline: colour index for values 0,1,2, and for 3 with and without bit 7
    rows = [[ci(nearest(l, 0, 0)), ci(nearest(l, 1, 0)), ci(nearest(l, 2, 0)),
             ci(nearest(l, 3, 0)), ci(nearest(l, 3, 1))] for l in range(len(table))]

    out = []
    out.append("/* hud.h - the game's HUD: its character set, colours and layout.\n"
               " *\n"
               " * Two ANTIC mode 4 rows of 40 characters at scanline %d, from $61C0, with the\n"
               " * character set at $6400. Colours were recovered by inverting a capture and\n"
               " * reproduce it exactly. Read out of a dump by extract_hud.py.\n"
               " * GENERATED FILE - do not edit. */\n" % HUD_TOP)
    out.append("#ifndef HUD_H_DATA\n#define HUD_H_DATA\n#include <stdint.h>\n")
    out.append("#define HUD_TOP %d\n#define HUD_COLS %d\n#define HUD_ROWS %d\n"
               "#define HUD_ROW_LINES %d\n#define HUD_LEFT %d\n#define HUD_CLOCKS_PER_PX %d\n"
               % (HUD_TOP, COLS, ROWS, ROW_LINES, LEFT, CLOCKS_PER_PX))
    for k, v in sorted(LAYOUT.items()):
        out.append("#define HUD_%s %d\n" % (k, v))
    for k, v in sorted(CHARS.items()):
        out.append("#define HUD_CH_%s 0x%02X\n" % (k, v))
    out.append("typedef struct { uint8_t r,g,b; } HudCol;\n")

    out.append("static const uint8_t HUD_FONT[%d][8]={\n" % NCHARS)
    for c in range(NCHARS):
        g = d[CHBASE + c * 8: CHBASE + c * 8 + 8]
        out.append("  {" + ",".join("0x%02X" % b for b in g) + "},\n")
    out.append("};\n")

    out.append("/* the two rows as the game had them, used as the starting template */\n")
    out.append("static const uint8_t HUD_TEMPLATE[%d]={\n" % (ROWS * COLS))
    for r in range(ROWS):
        out.append("  " + ",".join("0x%02X" % d[SCREEN + r * COLS + c]
                                   for c in range(COLS)) + ",\n")
    out.append("};\n")

    out.append("/* per scanline: palette index for pixel values 0,1,2, then value 3\n"
               " * without and with bit 7 of the character code (COLPF2 / COLPF3) */\n")
    out.append("static const uint8_t HUD_LINE_COL[%d][5]={\n  %s};\n"
               % (len(rows), ",".join("{%d,%d,%d,%d,%d}" % tuple(r) for r in rows)))
    out.append("static const HudCol HUD_PAL[%d]={%s};\n"
               % (len(pal), ",".join("{%d,%d,%d}" % c for c in pal)))
    out.append("/* the ippon markers ($324C), drawn as Players over the HUD at the\n"
               " * columns below; four rows of bitmap per marker */\n")
    out.append("static const uint8_t HUD_PIP[%d]={%s};\n"
               % (PIP_ROWS, ",".join("0x%02X" % d[PIP_DATA + k] for k in range(PIP_ROWS))))
    out.append("#define HUD_PIP_ROWS %d\n#define HUD_PIP_P1_COL %d\n"
               "#define HUD_PIP_P2_COL %d\n" % (PIP_ROWS, 7, 31))
    out.append("/* the belt names ($5EFF, offsets $5F53), already converted from the\n"
               " * ROM's storage by subtracting $36 as $5FB8 does */\n")
    belts = []
    for r in range(NBELTS):
        o = d[BELT_OFFSETS + r]
        belts.append([(d[BELT_TEXT + o + k] - 0x36) & 0xFF for k in range(BELT_LEN)])
    out.append("#define HUD_BELTS %d\n#define HUD_BELT_LEN %d\n" % (NBELTS, BELT_LEN))
    out.append("/* $5F66: the belt is a function of the SCORE, not of anything won.\n"
               " * $5F77 builds a byte from the score's second and third digits --\n"
               " * ((F7 & $0F) << 4) | (F8 >> 4), with the top digit clamped to 9 -- and\n"
               " * $5F95 walks these thresholds for the first one it is under. In two\n"
               " * players there is no belt at all ($5F6A). */\n")
    out.append("static const uint8_t HUD_BELT_THRESHOLD[HUD_BELTS]={%s};\n"
               % ",".join("0x%02X" % d[BELT_THRESHOLD + i] for i in range(NBELTS)))
    out.append("/* $5F60: the name's colour; 0 makes it flash from the clock ($5FAA) */\n")
    out.append("static const uint8_t HUD_BELT_COLOUR[HUD_BELTS]={%s};\n"
               % ",".join("0x%02X" % d[BELT_COLOUR + i] for i in range(NBELTS)))
    out.append("static const uint8_t HUD_BELT[HUD_BELTS][HUD_BELT_LEN]={\n%s};\n"
               % ",\n".join("  {" + ",".join("0x%02X" % c for c in b) + "}"
                            for b in belts))
    out.append("#endif\n")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").write("".join(out))
    print("wrote %s: %d palette entries" % (OUT, len(pal)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
