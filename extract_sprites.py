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
CAPTURES = sys.argv[1:] or ["extracted/colour4"]
NSHAPES = 54          # $558D-driven frames reference shape ids up to 51; the
                      # shape tables run to 53 and the ROM zeroes anything >= $36
CLOCKS_PER_PX = 2
GROUND_TOP = 128          # first scanline below the mode-E playfield
FRAME_H = 240

GI_RED = (132, 55, 63)
GI_WHITE = (211, 211, 211)
SKIN = (189, 113, 121)
OUTLINE = (4, 4, 4)
FX_ORIGIN = 28            # a fighter's screen origin in colour clocks is 2*x + 28

IDX_GI, IDX_SKIN, IDX_OUTLINE = 1, 2, 3


def frame(path):
    """Screenshot at one sample per colour clock."""
    return np.array(Image.open(path).convert("RGB")).astype(int)[::2, ::2]


def eq(im, c):
    return (im[:, :, 0] == c[0]) & (im[:, :, 1] == c[1]) & (im[:, :, 2] == c[2])


def fighter_isolate(im):
    """Isolate the red-gi fighter as a colour-index grid.

    The figure is the connected blob of gi and skin containing the red pixels -- which
    cannot bridge to the white fighter or the referee across the brown ground between
    them -- **grown outwards through the black**, so that everything the figure is drawn
    from comes along: the outline ring, the belt, and the hair.

    The hair is the reason this matters. It is pure black, so it is neither gi nor skin,
    and an earlier version took the gi+skin bounding box and added a single row of
    margin. A head carries three scanlines of hair above the first pixel of face, so two
    of them were cut off and every fighter came out flat-topped. Growing through the
    black instead recovers them and stops on its own: the propagation is masked to
    gi|skin|outline, and nothing black connects the figure to anything else.
    """
    gi, skin, out = eq(im, GI_RED), eq(im, SKIN), eq(im, OUTLINE)
    band = np.zeros(gi.shape, bool)
    band[GROUND_TOP:FRAME_H, :] = True
    gi, skin, out = gi & band, skin & band, out & band
    if not gi.any():
        return None
    lab, _ = ndimage.label(gi | skin, structure=np.ones((3, 3)))
    ids = set(lab[gi]) - {0}
    if not ids:
        return None
    seed = np.isin(lab, list(ids))
    figure = ndimage.binary_propagation(seed, structure=np.ones((3, 3)),
                                        mask=gi | skin | out)
    ys, xs = np.nonzero(figure)
    y0, y1 = ys.min(), ys.max()
    x0, x1 = xs.min(), xs.max()
    if (y1 - y0) > 60 or (x1 - x0) > 90:        # a leak, not a fighter
        return None

    width = (x1 - x0) // CLOCKS_PER_PX + 1
    grid = np.zeros((y1 - y0 + 1, width), np.uint8)
    for y in range(y0, y1 + 1):
        for k in range(width):
            x = x0 + k * CLOCKS_PER_PX
            if x > x1 or not figure[y, x]:
                continue                    # black that is not part of the figure
            if gi[y, x]:
                grid[y - y0, k] = IDX_GI
            elif skin[y, x]:
                grid[y - y0, k] = IDX_SKIN
            else:
                grid[y - y0, k] = IDX_OUTLINE
    return grid, y0, x0


def parity_ok(dump):
    """Whether the P/M double buffer was on its valid side when the dump was taken.

    $6119 alternates $08 (the fighter's four Players filled) and $18. Not every $18
    frame is spoiled and the dump is written a moment after the screenshot, so this is
    a preference rather than a filter -- see collect().
    """
    return len(dump) > 0x6120 and dump[0x6119] == 0x08


def collect(shape):
    """Every candidate pose for one shape id, as (grid, ytop, xoff, parity, name)."""
    cands = []
    shots = []
    for cdir in CAPTURES:
        shots += sorted(glob.glob(os.path.join(cdir, "s_%02X_*.png" % shape)))
    for shot in shots:
        dumppath = shot[:-4] + ".bin"
        if not os.path.exists(dumppath):
            continue
        d = open(dumppath, "rb").read()
        if len(d) <= 0x6120:
            continue
        res = fighter_isolate(frame(shot))
        if res is None:
            continue
        got, ytop, xleft = res
        if got.shape[1] < 3 or got.shape[0] < 6:
            continue                      # a fragment, not a pose
        xoff = xleft - (2 * d[0xE1] + 28)      # relative to the sprite's own origin
        cands.append((got, ytop, xoff, parity_ok(d), os.path.basename(shot)))
    return cands


# One Player is 8 sprite pixels wide; with its outline a single Player's worth of
# image measures about 10. A fighter is four adjacent Players, so anything this narrow
# is at most one quarter of a figure.
ONE_PLAYER_PX = 12


def artifacts(all_cands, min_shapes=3):
    """Bitmaps that cannot be poses: a partially composed frame.

    On a spoiled frame the fighter's four Players are not all filled and what is left on
    screen is one Player's worth -- a 9 px sliver, 53 scanlines tall. Two things give it
    away together. It is no wider than a single Player, so it cannot be a whole figure;
    and it is the same image whatever shape was poked, so it turns up under many
    different shape ids -- ten of them had adopted this one bitmap as their pose.

    Both conditions are needed. Recurrence alone is not enough: the neutral standing
    pose is genuinely shared by several move frames, and rejecting it threw away shape 0.
    """
    seen = {}
    for shape, cands in all_cands.items():
        for got, _y, _x, _p, _n in cands:
            seen.setdefault((got.shape, got.tobytes()), set()).add(shape)
    return {k for k, ids in seen.items()
            if len(ids) >= min_shapes and k[0][1] <= ONE_PLAYER_PX}


def dedupe(poses, weight, widths):
    """Two shapes cannot honestly share a bitmap, so when they do, one was never caught.

    The compositor at $4D30 reads a segment count out of $6BC0 for whatever is in $00DD
    -- 34 for shape 0, 15 for shape 48 -- and walks that many segments, so different
    shape ids compose different figures. An identical capture therefore means the poked
    id never took and what was recorded is whatever the fighter was already doing.

    That is what happened to shape 48, the upright half of the bow: it came out byte for
    byte the stand, with the stand's ink but an xoff 28 clocks adrift, because the dump it
    was measured against had the fighter somewhere else by then. Nothing noticed until the
    port started drawing the bow, and then the fighter jumped half a body width every time
    the pose came up.

    Rather than pass the bad geometry on, the copy is tied to the shape the capture really
    belongs to -- the one more captures agree on. The pose is then not measured, which the
    caller says out loud and the emitted header records. Fixing it properly needs a
    harvest that drives the game into the pose instead of poking $00DD (probe_bow.sh
    starts one), and the check below is what will notice if it ever comes back.
    """
    bybits = {}
    for s, (g, _ytop, _xoff) in poses.items():
        bybits.setdefault(g.tobytes(), []).append(s)
    aliased = []
    for group in bybits.values():
        if len(group) < 2:
            continue
        # Which of them the bitmap belongs to is decided by the ROM, not by how many
        # captures agree -- both counts are small and the wrong one can win. $5384 is the
        # arena-clamp width rather than the drawn one, but across the poses that came out
        # cleanly the drawn width is one more than it, so it identifies the owner: the
        # stand is 23 -> 24 and matches, shape 48 is 20 against the same 24 and does not.
        def owns(s):
            w = poses[s][0].shape[1]
            return 1 if widths and widths[s] + 1 == w else 0
        group.sort(key=lambda s: (-owns(s), -weight.get(s, 0), s))
        keep = group[0]
        if not owns(keep):
            print("shapes %s share a bitmap and $5384 does not say whose it is; "
                  "going by the captures" % group)
        for s in group[1:]:
            poses[s] = poses[keep]
            aliased.append((s, keep))
            print("shape %2d is byte for byte shape %2d (%d captures against %d): the poke "
                  "never took, so it is NOT a measurement of that pose -- tied to shape %d"
                  % (s, keep, weight.get(s, 0), weight.get(keep, 0), keep))
    if not aliased:
        print("no two shapes share a bitmap")
    return aliased


def pick(shape, cands, bad):
    """Best attempt for one shape: the pose the most captures agree on.

    Attempts disagree when a frame catches the fighter mid-transition or with something
    else clipped into view, so the winner is the grid the most attempts agree on rather
    than simply the biggest, with a valid-parity capture preferred to break ties.
    """
    cands = [c for c in cands if (c[0].shape, c[0].tobytes()) not in bad]
    if not cands:
        return None
    groups = {}
    for got, ytop, xoff, parity, name in cands:
        groups.setdefault((got.shape, got.tobytes()), []).append(
            (got, ytop, xoff, parity, name))
    # Valid parity outranks the head count. A spoiled frame drops part of the figure,
    # and several spoiled captures agree with each other precisely because they are all
    # missing the same thing: for the pose a struck fighter rests on, five $18 frames
    # outvoted three frames that included a valid one, and the pose came out with 191
    # ink pixels instead of 336 -- no outline and no head.
    best = max(groups.values(),
               key=lambda g: (sum(1 for c in g if c[3]), len(g),
                              int((g[0][0] > 0).sum())))
    got, ytop, xoff, parity, name = best[0]
    return got, name, len(best), len(cands), ytop, xoff


def measure_mirror(poses):
    """Where a pose's ink lands when the fighter faces RIGHT, measured.

    x0 is captured from the right-hand fighter, which faces left. The left-hand fighter
    in the same captures faces right and wears the white gi, so it gives the other half
    of the geometry directly instead of by assumption.

    The two are mirror images within the same Player/Missile field -- four adjacent
    double-width Players, 64 colour clocks -- so the offset facing right is
    `K - x0 - 2*width`, and K is what this measures. It came out as a firm constant, and
    an assumed value 56 clocks (28 sprite pixels) off had been drawing the right-facing
    fighter well to the right of where the game puts it.
    """
    votes = {}
    for cdir in CAPTURES:
        for shot in sorted(glob.glob(os.path.join(cdir, "s_*.png"))):
            m = re.search(r"s_([0-9A-F]{2})_", os.path.basename(shot))
            dumppath = shot[:-4] + ".bin"
            if not m or not os.path.exists(dumppath):
                continue
            sid = int(m.group(1), 16)
            if sid not in poses:
                continue
            d = open(dumppath, "rb").read()
            if len(d) <= 0x6120 or d[0x6119] != 0x08 or d[0xDD] != sid or d[0xE2] != 0:
                continue
            im = frame(shot)
            wh, sk, ol = eq(im, GI_WHITE), eq(im, SKIN), eq(im, OUTLINE)
            band = np.zeros(wh.shape, bool)
            # left of the right-hand fighter, below the playfield: the referee wears the
            # same white gi but stands to the right of both
            band[GROUND_TOP:FRAME_H, :2 * d[0xE1] + FX_ORIGIN - 8] = True
            wh, sk, ol = wh & band, sk & band, ol & band
            if not wh.any():
                continue
            lab, _ = ndimage.label(wh | sk, structure=np.ones((3, 3)))
            ids = set(lab[wh]) - {0}
            if not ids:
                continue
            fig = ndimage.binary_propagation(np.isin(lab, list(ids)),
                                             structure=np.ones((3, 3)), mask=wh | sk | ol)
            ys, xs = np.nonzero(fig)
            grid, y0, x0 = poses[sid]
            if ys.min() != y0:
                continue                  # not showing the poked pose
            k = xs.min() - (2 * d[0xE0] + FX_ORIGIN) + x0 + grid.shape[1] * CLOCKS_PER_PX
            votes[k] = votes.get(k, 0) + 1
    if not votes:
        return None, votes
    return max(votes, key=votes.get), votes


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

    ids = [s for s in range(NSHAPES) if not (heights and heights[s] == 0)]
    all_cands = {s: collect(s) for s in ids}
    bad = artifacts(all_cands)
    print("%d bitmap(s) rejected as spoiled-frame artifacts:" % len(bad))
    for shp, _blob in bad:
        print("   %dx%d, seen under several different shape ids" % (shp[1], shp[0]))
    print()

    poses, missing, weight = {}, [], {}
    for s in ids:
        got = pick(s, all_cands[s], bad)
        if got is None:
            missing.append(s)
            continue
        grid, src, agree, n, ytop, xoff = got
        # store facing right; the captured fighter (its $E3 is 1) faces left
        poses[s] = (grid[:, ::-1].copy(), ytop, xoff)
        weight[s] = agree
        print("shape %2d  %2dx%-2d px (ROM w=%2d) top=%3d xoff=%+d  %d/%d agree  %s"
              % (s, grid.shape[1], grid.shape[0], widths[s] if widths else 0,
                 ytop, xoff, agree, n, src))
    print("\nrecovered %d poses; missing %s" % (len(poses), missing or "none"))
    aliases = dedupe(poses, weight, widths)
    mirror, votes = measure_mirror(poses)
    print("mirror constant K (ink offset facing right = K - x0 - 2*width): %s"
          % ("%d clocks, %d of %d captures agree"
             % (mirror, votes[mirror], sum(votes.values())) if mirror is not None
             else "not measurable from these captures"))
    if poses:
        atlas(poses)
        emit(poses, mirror=mirror, aliases=aliases)
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


def emit(poses, aliases=(), path="generated/shapes_pm.h", mirror=None):
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
               )
    for s, keep in aliases:
        out.append(" *\n"
                   " * NOT MEASURED: shape %d. Its capture came back byte for byte shape %d's,\n"
                   " * which means the poked id never took -- $4D30 walks a different segment\n"
                   " * count for each ($6BC0), so two ids cannot compose the same figure. It\n"
                   " * carries shape %d's bitmap and geometry rather than the bad offset the\n"
                   " * capture measured, which was 28 clocks adrift and threw the fighter half a\n"
                   " * body width across the screen whenever the pose came up. To do it properly\n"
                   " * the game has to be driven into the pose instead of poked; probe_bow.sh\n"
                   " * starts that.\n" % (s, keep, keep))
    out.append(" * GENERATED FILE - do not edit; re-run extract_sprites.py instead. */\n")
    out.append("#ifndef SHAPES_PM_H\n#define SHAPES_PM_H\n#include <stdint.h>\n")
    out.append("#define SHAPE_COUNT %d\n" % NSHAPES)
    out.append("#define SHAPE_IDX_GI 1\n#define SHAPE_IDX_SKIN 2\n#define SHAPE_IDX_OUTLINE 3\n")
    out.append("/* y0 is the absolute scanline of the sprite's top row. The game gives a\n"
               " * fighter NO vertical motion -- jumps and falls are drawn into the poses\n"
               " * themselves -- so this is the whole of a sprite's vertical placement. */\n")
    out.append("/* x0 is the sprite's left edge as an offset in colour clocks from the\n"
               " * fighter's own screen origin, 2*x + 28; it varies per pose, and without it\n"
               " * the figure jitters horizontally as the animation runs. */\n")
    out.append("/* The colours the game actually puts on screen, read off the captures\n"
               " * rather than chosen: the two gi, the skin and the outline. */\n")
    for name, c in (("GI_WHITE", GI_WHITE), ("GI_RED", GI_RED),
                    ("SKIN", SKIN), ("OUTLINE", OUTLINE)):
        out.append("#define SHAPE_COL_%s %d,%d,%d\n" % (name, c[0], c[1], c[2]))
    if mirror is not None:
        out.append("/* x0 is captured from the fighter that faces LEFT. Facing right the\n"
                   " * pose is mirrored within the same four-Player field, so its ink\n"
                   " * starts at SHAPE_MIRROR_CLOCKS - x0 - 2*w from the origin. Measured\n"
                   " * from the left-hand fighter, which faces right. */\n")
        out.append("#define SHAPE_MIRROR_CLOCKS %d\n" % mirror)
    out.append("/* Shapes whose capture was not of them (see the note at the top): they carry\n"
               " * the geometry of the shape the bitmap belongs to, so their own captures are\n"
               " * not evidence about them and verify_sprites.py leaves them alone. */\n")
    out.append("#define SHAPE_ALIASED_COUNT %d\n" % len(aliases))
    out.append("static const uint8_t SHAPE_ALIASED[%d]={%s};\n"
               % (max(1, len(aliases)), ",".join(str(s) for s, _ in aliases) or "255"))
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
