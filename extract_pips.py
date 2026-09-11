#!/usr/bin/env python3
"""
Emit the HUD's ippon markers -- the bold dots that record a fighter's score.

Each player has three of them, and they are not a row: **two Points on the upper line
and one Half-Point below**, the lower one sitting under the right-hand of the pair. A
dot is drawn either dark or lit, and lighting is what records the score -- the two upper
dots fill right to left as full points are scored, and the lower one lights on its own
for a half point outstanding.

They are Player/Missile objects drawn over the HUD, not characters, so the geometry is
read off the screen: the dot's shape, the three positions, the two players' origins and
both colours. Nothing is typed in.

**Which dots light is not computed, it is a lookup.** $4514 takes the fighter's points
($00D9,y, 0..5), indexes $44A7 to get a start in $44AE, and copies eleven bytes -- eleven
scanlines of one Player, eight pixels wide -- into the player strip at offset $19:

    4514: LDA $44A5,Y / STA $63 / LDA #$00 / STA $62   ; $0400 or $0600
    451D: LDX $D9,Y / LDA $44A7,X / TAX
    4523: LDY #$19
    4525: LDA $44AE,X / STA ($62),Y / INX / INY / CPY #$24 / BCC

So this reads that table out of the dump too, rather than reconstructing the lighting
from the score. The port's old reconstruction (full = points/2, half = points & 1) does
agree with the table for all six values -- but it was a guess that happened to be right,
and the game has the answer.

Usage: python3 extract_pips.py [capture_dir ...] [--dump RAM.bin]
"""
import collections
import glob
import os
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

CAPTURES = sys.argv[1:] or ["../extracted/signs", "../extracted/scenes",
                            "../extracted/colour4"]
DARK = (104, 27, 35)
LIT = (250, 204, 144)
BAND_TOP, BAND_BOT = 8, 40          # the HUD band, well clear of the playfield
OUT = "generated/pips.h"

# $44A7: points -> where that pattern starts in $44AE. $44AE: eleven bytes per pattern.
POINT_INDEX = 0x44A7
POINT_BITMAP = 0x44AE
POINT_ROWS = 11
POINT_MAX = 5                       # $450B clamps $00D9,y here
DUMPS = ("../extracted/ram_true_64k.bin", "../extracted/fightdumps/dump_00.bin")


def load_point_table():
    """The six lit-dot patterns, read out of a RAM dump."""
    for path in DUMPS:
        try:
            d = open(path, "rb").read()
        except OSError:
            continue
        if len(d) <= POINT_BITMAP + 66:
            continue
        starts = [d[POINT_INDEX + p] for p in range(POINT_MAX + 1)]
        rows = [[d[POINT_BITMAP + s + r] for r in range(POINT_ROWS)] for s in starts]
        return os.path.basename(path), starts, rows
    return None, None, None


def frame(path):
    return np.array(Image.open(path).convert("RGB")).astype(int)[::2, ::2]


def eq(im, c):
    return (im[:, :, 0] == c[0]) & (im[:, :, 1] == c[1]) & (im[:, :, 2] == c[2])


def dots(im):
    """Every marker dot on screen as (x0, y0, w, h, lit), left to right.

    The two upper dots touch, so they come out as one blob and are split down the
    middle; the blob is exactly twice a dot wide, which is the check that it is a pair.
    """
    dark, lit = eq(im, DARK), eq(im, LIT)
    band = np.zeros(dark.shape, bool)
    band[BAND_TOP:BAND_BOT, :] = True
    m = (dark | lit) & band
    if not m.any():
        return []
    lab, n = ndimage.label(m, structure=np.ones((3, 3)))
    out = []
    for i in range(1, n + 1):
        ys, xs = np.nonzero(lab == i)
        w, h = xs.max() - xs.min() + 1, ys.max() - ys.min() + 1
        if h != 5 or w not in (8, 16):
            return []                       # not the markers; give up on this frame
        for part in range(w // 8):
            x0 = xs.min() + part * 8
            out.append((x0, ys.min(), 8, 5,
                        bool(lit[ys.min() + 2, x0 + 4])))
    return sorted(out)


def main():
    shots = []
    for d in CAPTURES:
        shots += sorted(glob.glob(os.path.join(d, "*.png")))
    if not shots:
        print("no captures present; skipping")
        return 1

    shapes = collections.Counter()
    layouts = collections.Counter()
    frames = []
    for p in shots:
        im = frame(p)
        d = dots(im)
        if len(d) != 6:                     # three per player
            continue
        m = eq(im, DARK) | eq(im, LIT)
        for x0, y0, w, h, _l in d:
            shapes[m[y0:y0 + h, x0:x0 + w].tobytes()] += 1
        layouts[tuple((x, y) for x, y, _w, _h, _l in d)] += 1
        frames.append((p, d))

    if not frames:
        print("no marker clusters found in %d captures" % len(shots))
        return 1
    if len(shapes) != 1:
        print("the dots are not all one shape (%d variants); refusing to emit"
              % len(shapes))
        return 1

    layout, seen = layouts.most_common(1)[0]
    if len(layouts) != 1:
        print("the markers are not always in the same place (%d layouts)" % len(layouts))
        return 1

    src, starts, patterns = load_point_table()
    if patterns is None:
        # The game's own data is not in this repository; harvest_v2.sh makes a dump.
        # See README.md.
        print("no RAM dump holding $44AE -- skipping")
        return 0
    print("point patterns from %s, $44A7 starts %s" % (src, starts))

    W, H = 8, 5
    grid = np.frombuffer(next(iter(shapes)), bool).reshape(H, W)
    p1 = layout[:3]
    p2 = layout[3:]
    ox1, oy = p1[0]
    ox2 = p2[0][0]
    offs = [(x - ox1, y - oy) for x, y in p1]
    if [(x - ox2, y - oy) for x, y in p2] != offs:
        print("the two players' markers are not laid out alike; refusing to emit")
        return 1

    print("%d captures show a full marker cluster, all agreeing" % len(frames))
    print("dot: %d colour clocks by %d scanlines" % (W, H))
    for r in grid:
        print("   " + "".join("#" if v else "." for v in r))
    print("positions relative to a player's own origin: %s" % (offs,))
    print("origins: player 1 at clock %d, player 2 at clock %d, scanline %d"
          % (ox1, ox2, oy))
    states = collections.Counter(tuple(l for _x, _y, _w, _h, l in d) for _p, d in frames)
    print("lit combinations seen (per player, in position order):")
    for s, n in states.most_common():
        print("   %s / %s   x%d" % (s[:3], s[3:], n))

    # Self-check: redraw every frame's cluster from the emitted data and insist it
    # matches the capture exactly, colours included.
    bad = 0
    for p, d in frames:
        im = frame(p)
        for x0, y0, _w, _h, lit in d:
            want = LIT if lit else DARK
            for y in range(H):
                for x in range(W):
                    if not grid[y, x]:
                        continue
                    if tuple(im[y0 + y, x0 + x]) != want:
                        bad += 1
    print("re-rendered over %d captures: %d differing pixels" % (len(frames), bad))
    if bad:
        print("refusing to emit")
        return 1

    # Turn each ROM pattern into "which of the three measured dots is lit". A Player is
    # eight bits wide and drawn double, so one ROM bit spans two colour clocks: the dot
    # measured at offset dx clocks covers ROM bits dx/2 .. dx/2+3. A dot is either wholly
    # present in the pattern or wholly absent, and that is checked here -- if it were
    # not, the measured geometry and the ROM table would disagree.
    CLOCKS_PER_BIT = 2
    lit_masks = []
    for pts, rows in enumerate(patterns):
        mask = 0
        for i, (dx, dy) in enumerate(offs):
            on = off = 0
            for r in range(H):
                for x in range(W):
                    if not grid[r, x]:
                        continue
                    bit = (rows[dy + r] >> (7 - (dx + x) // CLOCKS_PER_BIT)) & 1
                    on += bit
                    off += 1 - bit
            if on and off:
                print("points %d dot %d: the ROM pattern and the measured dot disagree "
                      "(%d bits on, %d off); refusing to emit" % (pts, i, on, off))
                return 1
            if on:
                mask |= 1 << i
        lit_masks.append(mask)
    print("$44AE -> which dots light, by points:")
    for pts, m in enumerate(lit_masks):
        print("   %d: %s" % (pts, "".join("*" if m & (1 << i) else "-"
                                          for i in range(len(offs)))))

    out = []
    out.append("/* pips.h - the HUD's ippon markers, captured from the screen.\n"
               " *\n"
               " * Two Points on the upper line and one Half-Point below it, per player,\n"
               " * the lower dot under the right-hand of the pair. A dot is drawn dark or\n"
               " * lit; the upper pair fills right to left with full points and the lower\n"
               " * one lights on its own for a half point outstanding.\n"
               " *\n"
               " * They are Player/Missile objects over the HUD, not characters, so they are\n"
               " * measured off the screen like the referee and his signs.\n"
               " * GENERATED FILE - do not edit; re-run extract_pips.py instead. */\n")
    out.append("#ifndef PIPS_H\n#define PIPS_H\n#include <stdint.h>\n")
    out.append("/* $450B clamps a fighter's points here */\n")
    out.append("#define PIP_POINTS_MAX %d\n" % POINT_MAX)
    out.append("/* $4514: which dots light is a lookup, not a calculation -- $44A7 picks\n"
               " * an eleven-scanline pattern out of $44AE by the fighter's points. These\n"
               " * are those six patterns, reduced to one bit per dot. */\n")
    out.append("static const uint8_t PIP_LIT[%d]={%s};\n"
               % (POINT_MAX + 1, ",".join("0x%02X" % m for m in lit_masks)))
    out.append("/* and the patterns themselves, as the ROM holds them */\n")
    out.append("static const uint8_t PIP_PATTERN[%d][%d]={\n"
               % (POINT_MAX + 1, POINT_ROWS))
    for rows in patterns:
        out.append("  {%s},\n" % ",".join("0x%02X" % b for b in rows))
    out.append("};\n")
    out.append("#define PIP_W %d\n#define PIP_H %d\n" % (W, H))
    out.append("#define PIP_COUNT %d\n" % len(offs))
    out.append("#define PIP_ORIGIN_P1 %d\n#define PIP_ORIGIN_P2 %d\n#define PIP_TOP %d\n"
               % (ox1, ox2, oy))
    out.append("/* the lower dot is the half point; the first two are the full points */\n")
    out.append("#define PIP_HALF_INDEX %d\n" % max(range(len(offs)),
                                                   key=lambda i: offs[i][1]))
    out.append("static const int8_t PIP_DX[PIP_COUNT]={%s};\n"
               % ",".join(str(dx) for dx, _dy in offs))
    out.append("static const int8_t PIP_DY[PIP_COUNT]={%s};\n"
               % ",".join(str(dy) for _dx, dy in offs))
    out.append("/* the colours the game puts on screen */\n")
    out.append("#define PIP_COL_DARK %d,%d,%d\n" % DARK)
    out.append("#define PIP_COL_LIT %d,%d,%d\n" % LIT)
    out.append("static const uint8_t PIP_PX[PIP_H*PIP_W]={%s};\n"
               % ",".join("1" if v else "0" for v in grid.flatten()))
    out.append("#endif\n")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").write("".join(out))
    print("wrote %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
