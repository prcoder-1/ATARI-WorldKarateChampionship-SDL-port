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
CAPTURES = sys.argv[1:] or ["../extracted/colour4"]
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


def trim_outline_edges(grid):
    """Drop edge rows and columns that are pure outline.

    A solid black line at the sprite's edge is the frame border or a neighbour clipped
    into view, never part of the figure."""
    def keep(vec):
        return (vec == IDX_GI).any() or (vec == IDX_SKIN).any()

    def solid_outline(vec):
        return (vec == IDX_OUTLINE).all()

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
    if top - 1 >= 0 and not solid_outline(grid[top - 1]):
        top -= 1
    if bot + 1 < grid.shape[0] and not solid_outline(grid[bot + 1]):
        bot += 1
    if lo - 1 >= 0 and not solid_outline(grid[:, lo - 1]):
        lo -= 1
    if hi + 1 < grid.shape[1] and not solid_outline(grid[:, hi + 1]):
        hi += 1
    out = grid[top:bot + 1, lo:hi + 1].copy()
    while out.shape[1] > 1 and solid_outline(out[:, 0]):
        out = out[:, 1:]
        lo += 1
    while out.shape[1] > 1 and solid_outline(out[:, -1]):
        out = out[:, :-1]
    return out, top, lo


def fighter_isolate(im):
    """Isolate the red-gi fighter as a colour-index grid.

    The earlier version cut a window out of $E0 plus the ROM's $5384 width. That was
    wrong twice over: the sprite reaches left of $E0, and with the fighters poked to the
    old positions the right-hand one sat against the edge of the playfield, so the screen
    clipped it as well. Nothing is assumed about the width now -- the figure is the
    connected blob of gi and skin containing the red pixels, which cannot bridge to the
    white fighter or the referee across the brown ground between them.
    """
    gi, skin, out = eq(im, GI_RED), eq(im, SKIN), eq(im, OUTLINE)
    band = np.zeros(gi.shape, bool)
    band[GROUND_TOP:FRAME_H, :] = True
    gi = gi & band
    if not gi.any():
        return None
    coloured = (gi | skin) & band
    lab, _ = ndimage.label(coloured, structure=np.ones((3, 3)))
    ids = set(lab[gi]) - {0}
    if not ids:
        return None
    comp = np.isin(lab, list(ids))
    ys, xs = np.nonzero(comp)
    y0, y1 = ys.min(), ys.max()
    x0, x1 = xs.min(), xs.max()
    if (y1 - y0) > 60 or (x1 - x0) > 90:        # a leak, not a fighter
        return None
    # one pixel of outline all round, where there is any
    y0 = max(y0 - 1, GROUND_TOP)
    y1 = min(y1 + 1, FRAME_H - 1)
    x0 = max(x0 - CLOCKS_PER_PX, 0)
    x1 = min(x1 + CLOCKS_PER_PX, gi.shape[1] - 1)

    width = (x1 - x0) // CLOCKS_PER_PX + 1
    grid = np.zeros((y1 - y0 + 1, width), np.uint8)
    for y in range(y0, y1 + 1):
        for k in range(width):
            x = x0 + k * CLOCKS_PER_PX
            if x > x1:
                break
            if gi[y, x]:
                grid[y - y0, k] = IDX_GI
            elif skin[y, x]:
                grid[y - y0, k] = IDX_SKIN
            elif out[y, x]:
                grid[y - y0, k] = IDX_OUTLINE
    grid, dtop, dleft = trim_outline_edges(grid)
    return grid, y0 + dtop, x0 + dleft * CLOCKS_PER_PX


def usable(dump_path):
    """Screen extraction does not care about the P/M double-buffer parity -- the
    screenshot shows whatever GTIA composited. Only $E1 is needed from the dump."""
    d = open(dump_path, "rb").read()
    return len(d) > 0x6120


def pick(shape, rom_w):
    """Best attempt for one shape.

    Attempts disagree when a frame catches the fighter mid-transition or with something
    else clipped into view, so the winner is the grid the most attempts agree on rather
    than simply the biggest. Only degenerate captures are dropped: some poses really are a few pixels wide,
    when the fighter is drawn edge-on mid-turn."""
    cands = []
    shots = []
    for cdir in CAPTURES:
        shots += sorted(glob.glob(os.path.join(cdir, "s_%02X_*.png" % shape)))
    for shot in shots:
        dump = shot[:-4] + ".bin"
        if not os.path.exists(dump) or not usable(dump):
            continue
        res = fighter_isolate(frame(shot))
        if res is None:
            continue
        got, ytop, xleft = res
        d = open(dump, "rb").read()
        xoff = xleft - (2 * d[0xE1] + 28)      # relative to the sprite's own origin
        if got.shape[1] < 3 or got.shape[0] < 6:
            continue                      # a fragment, not a pose
        cands.append((got, ytop, xoff, os.path.basename(shot)))
    if not cands:
        return None
    groups = {}
    for got, ytop, xoff, name in cands:
        key = (got.shape, got.tobytes())
        groups.setdefault(key, []).append((got, ytop, xoff, name))
    best = max(groups.values(),
               key=lambda g: (len(g), int((g[0][0] > 0).sum())))
    got, ytop, xoff, name = best[0]
    return int((got > 0).sum()), got, name, len(best) - 1, len(cands), ytop, xoff


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
        score, grid, src, agree, n, ytop, xoff = got
        # store facing right; the captured fighter (its $E3 is 1) faces left
        poses[s] = (grid[:, ::-1].copy(), ytop, xoff)
        print("shape %2d  %2dx%-2d px (ROM w=%2d) top=%3d xoff=%+d  %d/%d agree  %s"
              % (s, grid.shape[1], grid.shape[0], w, ytop, xoff, agree + 1, n, src))
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
    out.append("/* x0 is the sprite's left edge as an offset in colour clocks from the\n"
               " * fighter's own screen origin, 2*x + 28; it varies per pose, and without it\n"
               " * the figure jitters horizontally as the animation runs. */\n")
    out.append("typedef struct { uint8_t w, h, y0; int8_t x0; const uint8_t* px; } ShapePM;\n")
    for s in sorted(poses):
        g = poses[s][0]
        flat = ",".join(str(int(v)) for v in g.flatten())
        out.append("static const uint8_t shape%02d_px[%d]={%s};\n"
                   % (s, g.size, flat))
    ents = []
    for s in range(NSHAPES):
        if s in poses:
            g, ytop, xoff = poses[s]
            ents.append("  {%d,%d,%d,%d, shape%02d_px}"
                        % (g.shape[1], g.shape[0], ytop, xoff, s))
        else:
            ents.append("  {0,0,0,0, 0}")
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
