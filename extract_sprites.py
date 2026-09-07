#!/usr/bin/env python3
"""
Extract the game's fighter poses in colour, from the composited screen.

Why from the screen and not from Player/Missile memory: a fighter is four adjacent
Players and the two fighters share them, multiplexed into two scanline bands. A parked
fighter is never blanked, so "the occupied band" is ambiguous -- the first attempt at
this picked the wrong band and produced a mislabelled table (check_shape_ids.py caught
it). Driving BOTH fighters to the shape under test removes the ambiguity, and reading
the composited screen additionally recovers the colours, which the planes do not carry.

Geometry, measured from full-frame captures at exactly 2 host px per Atari colour clock:
a sprite pixel is 2 colour clocks wide and 1 scanline tall, so a fighter's four
double-width Players span 32 sprite pixels.

The two fighters wear different gi: the right-hand one is the only object on screen
drawn in the red gi colour, which makes it unambiguous to isolate (the left fighter and
the referee are both white). Pixels are emitted as colour *indices* -- 1 = gi, 2 = skin,
3 = outline -- so the port can paint each fighter its own gi colour.

Usage:  python3 extract_sprites.py [capture_dir]
"""
import glob
import os
import re
import struct
import sys
import zlib

import numpy as np
from PIL import Image
from scipy import ndimage

# Several capture runs can be merged: more attempts means more shapes land on a frame
# where the fighter is fully drawn.
CAPTURES = sys.argv[1:] or ["../extracted/colour3", "../extracted/colour2"]
NSHAPES = 54          # $558D-driven frames reference shape ids up to 51; the
                      # shape tables run to 53 and the ROM zeroes anything >= $36
CLOCKS_PER_PX = 2
GROUND_TOP = 128          # first scanline below the mode-E playfield
FRAME_H = 240

GI_RED = (132, 55, 63)
SKIN = (189, 113, 121)
OUTLINE = (4, 4, 4)

IDX_GI, IDX_SKIN, IDX_OUTLINE = 1, 2, 3


def frame(path):
    """Screenshot at one sample per colour clock."""
    return np.array(Image.open(path).convert("RGB")).astype(int)[::2, ::2]


def eq(im, c):
    return (im[:, :, 0] == c[0]) & (im[:, :, 1] == c[1]) & (im[:, :, 2] == c[2])


# Game X maps to screen colour clocks as clock = 2*x + 28, verified against $E0/$E1
# in the dumps. $E0/$E1 hold each fighter's LEFT EDGE and $5384 its width in sprite
# pixels, so a fighter's exact horizontal window is known without any guessing.
X_SCALE, X_ORIGIN = 2, 28


def fighter_window(im, left_edge, width):
    """Isolate the red fighter from its own x window.

    Vertical extent is grown out from the gi-coloured rows: that separates the fighter
    from the referee standing above it in the same column, and stops at the frame
    border below. Note $6BC0 is NOT the on-screen height -- it counts rows of the
    segment-encoded source data -- so only the width is validated against the ROM."""
    gi, skin, out = eq(im, GI_RED), eq(im, SKIN), eq(im, OUTLINE)
    x0 = X_SCALE * left_edge + X_ORIGIN
    cols = [x0 + X_SCALE * k for k in range(width)]
    if cols[-1] >= im.shape[1]:
        return None

    def has(mask, y):
        return any(mask[y, x] for x in cols)

    girows = [y for y in range(GROUND_TOP, FRAME_H) if has(gi, y)]
    if not girows:
        return None
    top, bot = min(girows), max(girows)
    # Grow only over COLOURED rows (gi or skin). Growing over outline pixels too runs
    # away: black appears somewhere in the window on nearly every row (referee, frame
    # border), which swallowed the whole band.
    coloured = lambda y: has(gi, y) or has(skin, y)
    while top - 1 >= GROUND_TOP and coloured(top - 1):
        top -= 1
    while bot + 1 < FRAME_H and coloured(bot + 1):
        bot += 1
    top = max(top - 2, GROUND_TOP)          # a little margin for the outline
    bot = min(bot + 2, FRAME_H - 1)
    h = bot - top + 1
    if h < 8 or h > 60:
        return None
    grid = np.zeros((h, width), np.uint8)
    for y in range(top, bot + 1):
        for k, x in enumerate(cols):
            if gi[y, x]:
                grid[y - top, k] = IDX_GI
            elif skin[y, x]:
                grid[y - top, k] = IDX_SKIN
            elif out[y, x]:
                grid[y - top, k] = IDX_OUTLINE
    grid, dtop = trim_outline_edges(grid)
    return grid, top + dtop


def trim_outline_edges(grid):
    """Drop edge rows/columns that are pure outline.

    The x window is the ROM's shape width, which can reach past the figure onto the
    frame border or a neighbouring object; those show up as solid black bars."""
    def keep(vec):
        return (vec == IDX_GI).any() or (vec == IDX_SKIN).any()

    top, bot = 0, grid.shape[0] - 1
    while top < bot and not keep(grid[top]):
        top += 1
    while bot > top and not keep(grid[bot]):
        bot -= 1
    lo, hi = 0, grid.shape[1] - 1
    while lo < hi and not keep(grid[:, lo]):
        lo += 1
    while hi > lo and not keep(grid[:, hi]):
        hi -= 1
    # Keep one row/column of outline around the figure, but never a line that is
    # SOLID outline: that is the frame border or a neighbouring object clipped by the
    # window, and it renders as a black bar down the sprite's edge.
    def solid_outline(vec):
        return (vec == IDX_OUTLINE).all()

    if top - 1 >= 0 and not solid_outline(grid[top - 1]):
        top -= 1
    if bot + 1 < grid.shape[0] and not solid_outline(grid[bot + 1]):
        bot += 1
    if lo - 1 >= 0 and not solid_outline(grid[:, lo - 1]):
        lo -= 1
    if hi + 1 < grid.shape[1] and not solid_outline(grid[:, hi + 1]):
        hi += 1
    out = grid[top:bot + 1, lo:hi + 1].copy()
    # and blank any remaining edge column that is pure outline
    while out.shape[1] > 1 and solid_outline(out[:, 0]):
        out = out[:, 1:]
        lo += 1
    while out.shape[1] > 1 and solid_outline(out[:, -1]):
        out = out[:, :-1]
    return out, top


def usable(dump_path):
    """Screen extraction does not care about the P/M double-buffer parity -- the
    screenshot shows whatever GTIA composited. Only $E1 is needed from the dump."""
    d = open(dump_path, "rb").read()
    return len(d) > 0x6120


def pick(shape, rom_w):
    """Best attempt for one shape, plus how many independent attempts agreed.

    Agreement between attempts is the correctness check here: two captures taken at
    different moments must yield the same pose for the same shape id."""
    cands = []
    shots = []
    for cdir in CAPTURES:
        shots += sorted(glob.glob(os.path.join(cdir, "s_%02X_*.png" % shape)))
    for shot in shots:
        dump = shot[:-4] + ".bin"
        if not os.path.exists(dump) or not usable(dump):
            continue
        d = open(dump, "rb").read()
        res = fighter_window(frame(shot), d[0xE1], rom_w)
        if res is None:
            continue
        got, ytop = res
        cands.append((int((got > 0).sum()), got, os.path.basename(shot), ytop))
    if not cands:
        return None
    cands.sort(key=lambda c: -c[0])
    best = cands[0]
    agree = sum(1 for c in cands[1:]
                if c[1].shape == best[1].shape and np.array_equal(c[1], best[1]))
    return best[0], best[1], best[2], agree, len(cands), best[3]


def main():
    heights = widths = None
    allbins = []
    for cdir in CAPTURES:
        allbins += sorted(glob.glob(os.path.join(cdir, "*.bin")))
    for p in allbins:
        d = open(p, "rb").read()
        if len(d) > 0x6BF0:
            heights = list(d[0x6BC0:0x6BC0 + NSHAPES])
            widths = list(d[0x5384:0x5384 + NSHAPES])
            break
    poses, missing = {}, []
    for s in range(NSHAPES):
        if heights and heights[s] == 0:
            continue                      # only shape 4: genuinely unused
        # $5384 is 0 for a few shapes; fall back to the full four-Player field.
        w = widths[s] or 32
        got = pick(s, w)
        if got is None:
            missing.append(s)
            continue
        score, grid, src, agree, n, ytop = got
        # store facing right; the captured fighter (its $E3 is 1) faces left
        poses[s] = (grid[:, ::-1].copy(), ytop)
        print("shape %2d  %2dx%-2d px (ROM w=%2d) top=%3d  %d/%d agree  %s"
              % (s, grid.shape[1], grid.shape[0], w, ytop, agree + 1, n, src))
    print("\nrecovered %d poses; missing %s" % (len(poses), missing or "none"))
    if poses:
        atlas(poses)
        emit(poses)
    return poses


def atlas(poses, path="screenshots/shape_atlas_colour.png", scale=3):
    cols = 8
    cw, ch = 34, 50
    rows = (len(poses) + cols - 1) // cols
    W, H = cols * cw, rows * ch
    pal = {0: (12, 12, 20), IDX_GI: GI_RED, IDX_SKIN: SKIN, IDX_OUTLINE: (0, 0, 0)}
    pix = [pal[0]] * (W * H)
    for i, s in enumerate(sorted(poses)):
        g = poses[s][0]
        cx, cy = (i % cols) * cw + 1, (i // cols) * ch + 1
        for y in range(min(g.shape[0], ch - 2)):
            for x in range(min(g.shape[1], cw - 2)):
                v = g[y, x]
                if v:
                    pix[(cy + y) * W + cx + x] = pal[v]
    _png(path, pix, W, H, scale)
    print("wrote", path)


def emit(poses, path="generated/shapes_pm.h"):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    out = []
    out.append("/* shapes_pm.h - the game's own fighter poses, in colour.\n"
               " *\n"
               " * Captured from the composited screen of a running World Karate Championship\n"
               " * (harvest_v2.sh drives BOTH fighters to the shape under test) and extracted\n"
               " * by extract_sprites.py. A sprite pixel is 2 colour clocks wide and 1 scanline\n"
               " * tall; a fighter spans four double-width Players, so at most 32 px.\n"
               " *\n"
               " * Pixels are colour INDICES, not colours: 0 = transparent, 1 = gi, 2 = skin,\n"
               " * 3 = outline. The gi colour differs per fighter, so the port supplies it.\n"
               " * Indexed by the game's own shape id, so FRAME_SHAPE[] indexes this directly.\n"
               " * GENERATED FILE - do not edit; re-run extract_sprites.py instead. */\n")
    out.append("#ifndef SHAPES_PM_H\n#define SHAPES_PM_H\n#include <stdint.h>\n")
    out.append("#define SHAPE_COUNT %d\n" % NSHAPES)
    out.append("#define SHAPE_IDX_GI 1\n#define SHAPE_IDX_SKIN 2\n#define SHAPE_IDX_OUTLINE 3\n")
    out.append("/* y0 is the absolute scanline of the sprite's top row. The game gives a\n"
               " * fighter NO vertical motion -- jumps and falls are drawn into the poses\n"
               " * themselves -- so this is the whole of a sprite's vertical placement. */\n")
    out.append("typedef struct { uint8_t w, h, y0; const uint8_t* px; } ShapePM;\n")
    for s in sorted(poses):
        g = poses[s][0]
        flat = ",".join(str(int(v)) for v in g.flatten())
        out.append("static const uint8_t shape%02d_px[%d]={%s};\n"
                   % (s, g.size, flat))
    ents = []
    for s in range(NSHAPES):
        if s in poses:
            g, ytop = poses[s]
            ents.append("  {%d,%d,%d, shape%02d_px}" % (g.shape[1], g.shape[0], ytop, s))
        else:
            ents.append("  {0,0,0, 0}")
    out.append("static const ShapePM SHAPE_PM[SHAPE_COUNT]={\n%s\n};\n" % ",\n".join(ents))
    out.append("#endif\n")
    open(path, "w").write("".join(out))
    print("wrote %s (%d poses)" % (path, len(poses)))


def _png(path, pix, W, H, sc):
    raw = bytearray()
    for y in range(H * sc):
        raw.append(0)
        for x in range(W * sc):
            raw += bytes(pix[(y // sc) * W + (x // sc)])

    def chunk(t, d):
        return (struct.pack(">I", len(d)) + t + d
                + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff))

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    open(path, "wb").write(
        b'\x89PNG\r\n\x1a\n'
        + chunk(b'IHDR', struct.pack(">IIBBBBB", W * sc, H * sc, 8, 2, 0, 0, 0))
        + chunk(b'IDAT', zlib.compress(bytes(raw), 9))
        + chunk(b'IEND', b''))


if __name__ == "__main__":
    main()
