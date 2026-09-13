#!/usr/bin/env python3
"""
Emit what each of the 256 GTIA colour bytes actually looks like, measured on the same
emulator every other colour in the port came off.

The port needs this because two things are stored as colour bytes rather than as pixels
anywhere: the belt colours at $5F60, and the black belt's, which the game builds every
frame out of the clock ($14 & $F0 | $0A -- see hud.h). Neither has a capture to be read
off the way the scenes and the HUD were, so the palette itself has to be measured.

Input: two full-frame grabs from harvest_palette.sh, in each of which the machine has
been taken over and paints COLBK one scanline at a time from a known starting byte.

Working out which scanline is which byte is done from the picture, not assumed: the low
bit of a colour register is ignored by GTIA, so bytes come in identical pairs, and hue 0
is the grey ramp, so the run of exactly-grey rows the first pass opens with is bytes
$00..$0F. Where that run ends fixes the offset.

Then the two passes are checked against each other. They overlap over most of the range,
and every byte they share has to come out the same, every pair 2n/2n+1 has to be
identical, and all 256 have to be covered, or nothing is emitted.

Usage: python3 extract_palette.py [dir-with-ramp_00.png-and-ramp_80.png]
"""
import os
import sys

try:
    from PIL import Image
except ImportError:
    print("PIL is not installed -- skipping")
    raise SystemExit(0)

CAPTURES = sys.argv[1] if len(sys.argv) > 1 else "extracted/palette"
OUT = "generated/palette.h"

PASSES = ((0x00, "ramp_00.png"), (0x80, "ramp_80.png"))
ROWS = 240              # visible Atari scanlines in a full-frame grab
RAMP = 240              # scanlines the harvester paints per pass: its LDY #$F0
SAMPLE = (120, 264)     # colour clocks to read a row's colour from, well off both edges


def rows_of(path):
    """One RGB per visible scanline, taken from the middle of the line."""
    im = Image.open(path).convert("RGB")
    im = im.resize((im.width // 2, im.height // 2), Image.NEAREST)
    px = im.load()
    out = []
    for y in range(min(ROWS, im.height)):
        seen = {}
        for x in range(*SAMPLE):
            c = px[x, y]
            seen[c] = seen.get(c, 0) + 1
        out.append(max(seen, key=seen.get))
    return out


def offset_from_grey_run(rows):
    """The first pass starts at byte $00, and hue 0 is the greys, so the opening run of
    exactly-grey rows is the tail of bytes $00..$0F. Its last row is byte $0F."""
    n = 0
    while n < len(rows) and rows[n][0] == rows[n][1] == rows[n][2]:
        n += 1
    if n == 0 or n > 16:
        return None
    return 0x0F - (n - 1)


def main():
    paths = [os.path.join(CAPTURES, f) for _, f in PASSES]
    if not all(os.path.exists(p) for p in paths):
        print("no palette ramps present -- run harvest_palette.sh first; skipping")
        return 0

    first = rows_of(paths[0])
    off = offset_from_grey_run(first)
    if off is None:
        print("the first pass does not open with a grey run; refusing to guess the offset")
        return 1
    print("row 0 of the ramps is colour byte $%02X (%d grey rows lead it)" % (off, 0x10 - off))

    # The ramp begins above the visible area -- row 0 is already its `off`th scanline --
    # so its last `off` steps fall off the bottom, and those rows still hold whatever
    # colour the previous frame left. They are not measurements; drop them.
    usable = RAMP - off
    pal = {}
    for (start, fname), path in zip(PASSES, paths):
        for y, rgb in enumerate(rows_of(path)[:usable]):
            b = (start + off + y) & 0xFF
            if b in pal and pal[b] != rgb:
                print("$%02X is %s in one pass and %s in the other" % (b, pal[b], rgb))
                return 1
            pal[b] = rgb

    missing = [b for b in range(256) if b not in pal]
    if missing:
        print("%d colour bytes never made it on screen: %s"
              % (len(missing), " ".join("$%02X" % b for b in missing[:8])))
        return 1

    odd = [b for b in range(0, 256, 2) if pal[b] != pal[b + 1]]
    if odd:
        print("GTIA ignores a colour register's low bit, but %d pairs differ: %s"
              % (len(odd), " ".join("$%02X" % b for b in odd[:8])))
        return 1

    shared = sum(1 for b in range(256))
    print("all 256 bytes measured, both passes agreeing, %d distinct colours"
          % len({pal[b] for b in range(256)}))

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    out = []
    out.append("/* generated/palette.h - what the 256 GTIA colour bytes look like.\n"
               " *\n"
               " * Measured, not computed: harvest_palette.sh takes the machine over and paints\n"
               " * COLBK one scanline at a time, and extract_palette.py reads the frame back. So\n"
               " * these are the same colours the scene and HUD captures were taken through.\n"
               " *\n"
               " * GTIA ignores the low bit, which is why 2n and 2n+1 are always equal.\n"
               " *\n"
               " * GENERATED FILE - do not edit; re-run extract_palette.py instead. */\n")
    out.append("#ifndef GENERATED_PALETTE_H")
    out.append("#define GENERATED_PALETTE_H")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    out.append("typedef struct { uint8_t r, g, b; } AtariCol;")
    out.append("")
    out.append("static const AtariCol ATARI_PAL[256] = {")
    for base in range(0, 256, 8):
        row = ", ".join("{%3d,%3d,%3d}" % pal[b] for b in range(base, base + 8))
        out.append("  %s,   /* $%02X */" % (row, base))
    out.append("};")
    out.append("")
    out.append("#endif")
    with open(OUT, "w") as f:
        f.write("\n".join(out) + "\n")
    print("wrote %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
