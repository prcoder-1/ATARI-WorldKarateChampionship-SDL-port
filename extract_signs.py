#!/usr/bin/env python3
"""
Emit the referee's announcement signs as C data.

The referee does not only stand there: he holds up a sign. It is a white board with
black lettering and a brown handle, and it says what has just happened -- BEGIN at the
start of a bout, HALF POINT and FULL POINT for a score, RED and WHITE for who scored,
a bonus figure at the end.

Like the referee himself the sign is captured from the screen rather than decoded. It is
not text in the display's character memory: the $0800 region that covers the whole
ground (twelve ANTIC mode 4 rows, scanlines 137..232, per the display list at $62CC) is
blank in every dump where a sign is on screen, so the sign is a Player/Missile object.
Nor is the wording anywhere in RAM or on the disk, in ASCII or in the game's own
character codes -- the disk data is packed.

Every sign lands in the same place, 48 colour clocks by 13 scanlines at clock 264,
scanline 145, which is what tells a real sign from a fallen fighter's white gi.

Usage: python3 extract_signs.py [capture_dir ...]
"""
import glob
import hashlib
import os
import re
import struct
import sys
import zlib

import numpy as np
from PIL import Image
from scipy import ndimage

CAPTURES = sys.argv[1:] or ["../extracted/signs", "../extracted/endgame",
                            "../extracted/scenes", "../extracted/colour4",
                            "../extracted/harvest2"]
BOARD = (211, 211, 211)
INK = (4, 4, 4)
IDX_BOARD, IDX_INK = 1, 2

SIGN_X, SIGN_Y = 264, 145          # where every sign appears, measured
SIGN_W, SIGN_H = 48, 13            # colour clocks by scanlines
CLOCKS_PER_PX = 2
OUT = "generated/signs.h"

# What each captured sign says. The pixels are generated; these names are read off the
# rendered bitmaps by eye, which is the only way to get them -- the wording exists
# nowhere in RAM or on the disk to be decoded. Keyed by the bitmap so a re-run cannot
# shuffle them, and a sign that turns up without a name here is emitted unnamed rather
# than guessed at.
LABELS = {
    "2f22bafd8772ba7b564f135d99a3dd4e": "BEGIN",
    "5917e67b09c764c95925a586ed3d3e8f": "WHITE",
    "c56a6a33e7c84bb4925f88bd67bc2af0": "FULL POINT",
    "28fed1ba497b81633238ad17d548b6e8": "",            # blank board
    "95e16e52d6e9fc34174ff0eafc180f43": "RED",
    "9bdc897dd674a2ab319ff73b1bf47d41": "2600 BONUS",
    "24272c9257c75e57261007a4e7a76e3a": "2000 BONUS",
    "3d5d0206de4adedb23d6da714a0b7bfa": "HALF POINT",
    "3331bdd760eaeb8c579806b8a5d4f89c": "1800 BONUS",
    "a865880c0b1903ae262bc1a4232b1248": "2100 BONUS",
    "45d7883d98fe93dcaa9ee2fec154c32c": "MATCH OVER",
}


def frame(path):
    return np.array(Image.open(path).convert("RGB")).astype(int)[::2, ::2]


def eq(im, c):
    return (im[:, :, 0] == c[0]) & (im[:, :, 1] == c[1]) & (im[:, :, 2] == c[2])


def isolate(im):
    """The sign as a colour-index grid, or None when no sign is on screen."""
    board = eq(im, BOARD)
    band = np.zeros(board.shape, bool)
    band[130:175, :318] = True            # the referee's own band, left of him
    b = board & band
    if not b.any():
        return None
    lab, n = ndimage.label(b, structure=np.ones((3, 3)))
    for i in range(1, n + 1):
        ys, xs = np.nonzero(lab == i)
        if (xs.min(), ys.min()) != (SIGN_X, SIGN_Y):
            continue
        if (xs.max() - xs.min() + 1, ys.max() - ys.min() + 1) != (SIGN_W, SIGN_H):
            continue
        ink = eq(im, INK)
        w = SIGN_W // CLOCKS_PER_PX
        g = np.zeros((SIGN_H, w), np.uint8)
        for y in range(SIGN_H):
            for k in range(w):
                x = SIGN_X + k * CLOCKS_PER_PX
                sy = SIGN_Y + y
                # Anything that is neither board nor lettering is the ground showing
                # through the sign's cut corner, and the ground differs from scene to
                # scene -- treating it as a colour made one sign into several.
                if board[sy, x]:
                    g[y, k] = IDX_BOARD
                elif ink[sy, x]:
                    g[y, k] = IDX_INK
        return g
    return None


def main():
    shots = []
    for d in CAPTURES:
        shots += sorted(glob.glob(os.path.join(d, "*.png")))
    if not shots:
        print("no captures present; skipping")
        return 1

    seen = {}
    for p in shots:
        g = isolate(frame(p))
        if g is None:
            continue
        seen.setdefault(hashlib.md5(g.tobytes()).hexdigest(), []).append((g, p))
    # the tiebreak is the full path: harvest2 and colour4 hold files of the same name
    keys = sorted(seen, key=lambda k: (-len(seen[k]), seen[k][0][1]))

    if not seen:
        print("no signs found in %d captures" % len(shots))
        return 1

    # Never replace the emitted table with a smaller one. The captures are not in this
    # repository (see README.md), and only a few of them come with the scene grabs that
    # are -- so running this with a partial set would quietly cut the eleven signs down
    # to whichever happened to be lying around, taking MATCH OVER and the rest with them.
    try:
        have = re.search(r"#define SIGN_COUNT (\d+)", open(OUT).read())
    except OSError:
        have = None
    if have and len(seen) < int(have.group(1)):
        print("found %d signs but %s already has %d -- skipping rather than shrink it"
              % (len(seen), OUT, int(have.group(1))))
        return 0

    # most-seen first, so the ordering is stable across runs
    order = [seen[k] for k in keys]
    print("%d distinct signs over %d captures" % (len(order), len(shots)))
    unnamed = [k for k in keys if k not in LABELS]
    if unnamed:
        print("  %d sign(s) not in LABELS, emitted unnamed: %s"
              % (len(unnamed), ", ".join(k[:8] for k in unnamed)))
    for k, group in zip(keys, order):
        g, name = group[0]
        print("  %-11s seen %3d times (%s)"
              % ('"%s"' % LABELS[k] if k in LABELS else "unnamed", len(group),
                 os.path.basename(name)))
        for r in g:
            print("     " + "".join(" #."[v] for v in r))

    out = []
    out.append("/* signs.h - the referee's announcement signs, captured from the screen.\n"
               " *\n"
               " * He holds one up to say what has happened: BEGIN, HALF POINT, FULL POINT,\n"
               " * RED or WHITE for who scored, a bonus figure at the end. Every sign is the\n"
               " * same size and lands in the same place, beside him.\n"
               " *\n"
               " * Captured rather than decoded: the wording is nowhere in RAM or on the disk,\n"
               " * in ASCII or in the game's own character codes, and the $0800 character\n"
               " * memory that covers the ground is blank while a sign is showing -- so the\n"
               " * sign is a Player/Missile object, like the referee himself.\n"
               " * GENERATED FILE - do not edit; re-run extract_signs.py instead. */\n")
    out.append("#ifndef SIGNS_H\n#define SIGNS_H\n#include <stdint.h>\n")
    out.append("#define SIGN_COUNT %d\n" % len(order))
    out.append("#define SIGN_W %d\n#define SIGN_H %d\n"
               % (SIGN_W // CLOCKS_PER_PX, SIGN_H))
    out.append("#define SIGN_X %d\n#define SIGN_Y %d\n" % (SIGN_X, SIGN_Y))
    out.append("#define SIGN_CLOCKS_PER_PX %d\n" % CLOCKS_PER_PX)
    out.append("#define SIGN_IDX_BOARD %d\n#define SIGN_IDX_INK %d\n"
               % (IDX_BOARD, IDX_INK))
    out.append("/* the colours the game puts on screen, read off the captures */\n")
    for name, c in (("BOARD", BOARD), ("INK", INK)):
        out.append("#define SIGN_COL_%s %d,%d,%d\n" % (name, c[0], c[1], c[2]))
    out.append("/* what each sign says, read off the bitmaps; \"\" is the blank board */\n")
    out.append("static const char* const SIGN_NAME[SIGN_COUNT]={%s};\n"
               % ",".join('"%s"' % LABELS.get(k, "") for k in keys))
    for i, group in enumerate(order):
        g = group[0][0]
        out.append("static const uint8_t sign%02d_px[SIGN_W*SIGN_H]={%s};\n"
                   % (i, ",".join(str(int(v)) for v in g.flatten())))
    out.append("static const uint8_t* const SIGN_PX[SIGN_COUNT]={\n  %s};\n"
               % ",".join("sign%02d_px" % i for i in range(len(order))))
    out.append("#endif\n")
    # Self-check before emitting, the way extract_hud.py does: draw each sign back over
    # every capture it came from, at the fixed place, and insist on an exact match.
    bad = 0
    checked = 0
    for k, group in zip(keys, order):
        g = group[0][0]
        for _g, path in group:
            im = frame(path)
            checked += 1
            for y in range(SIGN_H):
                for x in range(SIGN_W // CLOCKS_PER_PX):
                    v = g[y, x]
                    if not v:
                        continue
                    want = BOARD if v == IDX_BOARD else INK
                    for c in range(CLOCKS_PER_PX):
                        if tuple(im[SIGN_Y + y, SIGN_X + x * CLOCKS_PER_PX + c]) != want:
                            bad += 1
    print("re-rendered over %d captures: %d differing pixels" % (checked, bad))
    if bad:
        print("refusing to emit")
        return 1

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").write("".join(out))
    print("wrote %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
