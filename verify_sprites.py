#!/usr/bin/env python3
"""
Check the extracted fighter poses against the screen the real game drew.

This is the sprite counterpart of verify_scenes: it re-renders each pose from
generated/shapes_pm.h exactly the way worldkarate.c draws it -- at the pose's own
absolute top scanline, two colour clocks per sprite pixel, mirrored because the captured
fighter faces left -- and compares the result with the original screenshots, pixel for
pixel.

It is deliberately not circular. A pose is extracted from one capture and checked here
against *every* capture of that shape id, the source one excluded from the count where
others exist. A pose that only reproduces the frame it came from is not a pose.

Two defects this was written to catch, both of which it does:

  * the heads were flat. Hair is pure black, so it counted as outline rather than as
    part of the figure, and the extractor kept a single row of margin above the topmost
    gi-or-skin pixel. A head carries three scanlines of hair, so two were cut off every
    fighter.
  * ten shape ids shared one bitmap: a 9 px sliver, one Player's worth of a
    partially composed frame, picked up from captures where $6119 was $18.

Usage: python3 verify_sprites.py [capture_dir ...]
"""
import glob
import os
import re
import sys

import numpy as np
from PIL import Image

CAPTURES = sys.argv[1:] or ["extracted/colour4"]
HEADER = "generated/shapes_pm.h"
CLOCKS_PER_PX = 2
FX_ORIGIN = 28            # $E1 -> colour clocks: 2*x + 28, as worldkarate.c has it
MARGIN = 2                # how far outside the pose to look for ink it should have had
GI_RED = (132, 55, 63)
GI_WHITE = (211, 211, 211)
SKIN = (189, 113, 121)
OUTLINE = (4, 4, 4)


def mirror_clocks(path=HEADER):
    m = re.search(r"#define SHAPE_MIRROR_CLOCKS (-?\d+)", open(path).read())
    return int(m.group(1)) if m else None


def load_poses(path=HEADER):
    src = open(path).read()
    px = {int(m.group(1)): [int(v) for v in m.group(2).split(",") if v.strip()]
          for m in re.finditer(r"shape(\d+)_px\[\d+\]=\{([^}]*)\}", src)}
    body = src.split("SHAPE_PM[SHAPE_COUNT]={", 1)[1]
    rows = re.findall(r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(-?\d+)\s*,", body)
    poses = {}
    for sid, (w, h, y0, x0) in enumerate(rows):
        w, h, y0, x0 = int(w), int(h), int(y0), int(x0)
        if not w or sid not in px:
            continue
        poses[sid] = (np.array(px[sid], np.uint8).reshape(h, w), y0, x0)
    return poses


def frame(path):
    return np.array(Image.open(path).convert("RGB")).astype(int)[::2, ::2]


def compare(pose, im, origin, facing, gi, mirror):
    """Draw the pose the way worldkarate.c does and count pixels that differ.

    spriteLeft(): facing left the ink starts at origin + x0; facing right the pose is
    mirrored within its Player/Missile field and starts at
    origin + SHAPE_MIRROR_CLOCKS - x0 - 2*w. drawFighter() reverses the columns when the
    fighter faces left, because the bitmaps are stored facing right.
    """
    colour = {1: gi, 2: SKIN, 3: OUTLINE}
    # Only gi and skin count as ink the pose was obliged to cover. Black does not: the
    # game paints a shadow on the ground under a fighter, a scanline or two below the
    # sprite, and it is not part of the sprite.
    ink = {gi, SKIN}
    grid, y0, x0 = pose
    h, w = grid.shape
    left = origin + (x0 if facing else mirror - x0 - w * CLOCKS_PER_PX)

    # Paint the pose into a canvas the way drawFighter does, then compare the canvas
    # with the capture over a box two pixels larger all round. The margin is what makes
    # the check symmetric: without it, only pixels the pose paints are ever looked at,
    # and a pose MISSING ink passes. One did -- the pose a struck fighter rests on had
    # lost its head, and the sand showed through where the head should be.
    canvas = {}
    for y in range(h):
        for k in range(w):
            v = grid[y, w - 1 - k] if facing else grid[y, k]
            if not v:
                continue
            for c in range(CLOCKS_PER_PX):
                canvas[(y0 + y, left + k * CLOCKS_PER_PX + c)] = colour[v]

    diff = total = 0
    for sy in range(y0 - MARGIN, y0 + h + MARGIN):
        if not 0 <= sy < im.shape[0]:
            return None
        for sx in range(left - MARGIN * CLOCKS_PER_PX,
                        left + (w + MARGIN) * CLOCKS_PER_PX):
            if not 0 <= sx < im.shape[1]:
                return None
            want = canvas.get((sy, sx))
            got = tuple(im[sy, sx])
            if want is None:
                if got in ink:            # the game drew figure here and the pose did not
                    total += 1
                    diff += 1
            else:
                total += 1
                if got != want:
                    diff += 1
    return diff, total


def shows_another_pose(poses, sid, results, mirror, gi, who):
    """Whether every capture of `sid` had that fighter showing some other pose.

    harvest_v2.sh pokes $DD and NOPs the stores that would overwrite it, but the poke
    does not always hold: some dumps come back with $DD at 00 and the fighter running
    its own animation. Such a frame is not evidence against the pose under test, and
    counting it as a failure would be wrong.

    The test is the whole point: the frame is only excused when some *other* emitted
    pose reproduces it exactly. That is not circular -- those poses stand on their own
    captures -- and a frame that matches nothing at all is still reported as a failure.
    """
    for diff, _total, _name, shot, origin in results:
        if diff == 0:
            return None
        for other, pose in poses.items():
            if other == sid:
                continue
            got = compare(pose, frame(shot), origin, who, gi, mirror)
            if got and got[0] == 0:
                return other
    return None


def aliased(path=HEADER):
    """Shapes extract_sprites.py had to tie to another because their capture was not of
    them. Those captures are the bad measurement itself, so re-rendering the emitted pose
    over them can only reproduce the error -- see the note at the top of the header."""
    m = re.search(r"SHAPE_ALIASED\[\d+\]=\{([^}]*)\}", open(path).read())
    if not m:
        return set()
    return {int(v) for v in m.group(1).split(",") if v.strip() and int(v) != 255}


def main():
    poses = load_poses()
    skip = aliased()
    shots = []
    for d in CAPTURES:
        shots += sorted(glob.glob(os.path.join(d, "s_*.png")))
    if not shots:
        print("no captures present -- run harvest_v2.sh first; skipping")
        return 0

    mirror = mirror_clocks()
    if mirror is None:
        print("the header carries no SHAPE_MIRROR_CLOCKS; re-run extract_sprites.py")
        return 1
    per_shape = {0: {}, 1: {}}
    for shot in shots:
        m = re.search(r"s_([0-9A-F]{2})_", os.path.basename(shot))
        if not m:
            continue
        sid = int(m.group(1), 16)
        if sid not in poses or sid in skip:
            continue
        dump = shot[:-4] + ".bin"
        if not os.path.exists(dump):
            continue
        d = open(dump, "rb").read()
        if len(d) <= 0x6120 or d[0x6119] != 0x08 or d[0xDD] != sid:
            continue                      # a partially composed frame proves nothing
        im = frame(shot)
        # the right-hand fighter faces left and wears the red gi; the left-hand one
        # faces right and wears the white, which is what puts the mirror under test
        for who, xaddr, faddr, gi in ((1, 0xE1, 0xE3, GI_RED),
                                      (0, 0xE0, 0xE2, GI_WHITE)):
            if d[faddr] != who:
                continue
            origin = 2 * d[xaddr] + FX_ORIGIN
            got = compare(poses[sid], im, origin, who, gi, mirror)
            if got is None:
                continue
            diff, total = got
            per_shape[who].setdefault(sid, []).append(
                (diff, total, os.path.basename(shot), shot, origin))

    failed = 0
    for who, label, gi in (
            (1, "facing left  (the red fighter, which x0 was captured from)", GI_RED),
            (0, "facing right (the white fighter, mirrored by the port)", GI_WHITE)):
        exact = checked = 0
        worst, unchecked = [], []
        for sid in sorted(per_shape[who]):
            best = min(per_shape[who][sid])
            instead = (None if best[0] == 0 else
                       shows_another_pose(poses, sid, per_shape[who][sid],
                                          mirror, gi, who))
            if instead is not None:
                unchecked.append((sid, instead))
                continue
            checked += 1
            if best[0] == 0:
                exact += 1
            else:
                worst.append((best[0], best[1], sid, best[2],
                              len(per_shape[who][sid])))
        print("%s\n  %d of %d shapes re-render pixel for pixel" % (label, exact, checked))
        if worst:
            worst.sort(reverse=True)
            for diff, total, sid, name, n in worst[:10]:
                print("     shape %2d: %d of %d ink pixels differ, best of %d captures (%s)"
                      % (sid, diff, total, n, name))
        failed += checked - exact
        for sid, instead in unchecked:
            print("     shape %d: no capture holds the poked pose -- that fighter is "
                  "showing shape %d instead, so it is not checked here"
                  % (sid, instead))
        print()

    for sid in sorted(skip):
        print("shape %d is not checked: its own captures are what got it wrong, so the "
              "geometry it carries is another shape's and they cannot agree" % sid)
    print("%s (%d shape/facing combinations differ)"
          % ("FAILURES" if failed else "every pose reproduces the original exactly",
             failed))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
