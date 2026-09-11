#!/usr/bin/env python3
"""
Cross-check that the poses in generated/shapes_pm.h really are the shapes they claim.

The harvest parks one fighter on unused shape 4 and pokes the other to shape N. But a
parked fighter is NOT blanked -- the P/M buffer keeps its last pose -- so "the occupied
band" can belong to either fighter. If pick_shapes.py ever chose the wrong band, the
whole table is mislabelled.

This compares, for each capture, the fighter actually visible ON SCREEN against every
extracted silhouette, and reports which index matches best. A correct table gives
best == poked shape for (almost) every capture.

Usage: python3 check_shape_ids.py
"""
import glob
import os
import re
import sys

import numpy as np
from PIL import Image
from scipy import ndimage

GI = (132, 55, 63)
SKIN = (189, 113, 121)
BLACK = (4, 4, 4)
CLOCKS_PER_PX = 2          # a sprite pixel is two colour clocks wide
COLOUR_DIR = "extracted/colour"


def load_pm_table(path="generated/shapes_pm.h"):
    """Parse the generated header back into {shape: boolean mask}."""
    src = open(path).read()
    out = {}
    for m in re.finditer(r"shape(\d+)_rows\[(\d+)\]\[4\]=\{(.*?)\};", src, re.S):
        sid, h, body = int(m.group(1)), int(m.group(2)), m.group(3)
        vals = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", body)]
        grid = np.zeros((h, 32), bool)
        for y in range(h):
            for p in range(4):
                b = vals[y * 4 + p]
                for k in range(8):
                    grid[y, p * 8 + k] = bool(b & (1 << (7 - k)))
        out[sid] = trim(grid)
    return out


def trim(mask):
    ys, xs = np.nonzero(mask)
    if len(xs) == 0:
        return mask
    return mask[ys.min():ys.max() + 1, xs.min():xs.max() + 1]


def screen_fighter(shot_path):
    """The largest on-screen figure drawn in the fighters' own colours."""
    im = np.array(Image.open(shot_path).convert("RGB")).astype(int)[::2, ::2]

    def eq(c):
        return (im[:, :, 0] == c[0]) & (im[:, :, 1] == c[1]) & (im[:, :, 2] == c[2])

    body = eq(GI) | eq(SKIN)
    if not body.any():
        return None
    lab, n = ndimage.label(body | eq(BLACK), structure=np.ones((3, 3)))
    # score components by how much *body* colour they contain: that excludes the
    # black HUD text and keeps figures
    best, bestn = None, 0
    for i in range(1, n + 1):
        comp = lab == i
        score = (comp & body).sum()
        if score > bestn:
            best, bestn = comp, score
    if best is None:
        return None
    sub = trim(best)
    # collapse the doubled colour clocks back to sprite pixels
    return sub[:, ::CLOCKS_PER_PX]


def iou(a, b):
    """Best overlap of two masks over all alignments, as intersection-over-union."""
    H = max(a.shape[0], b.shape[0]) + 4
    W = max(a.shape[1], b.shape[1]) + 4
    best = 0.0
    for dy in range(-3, 4):
        for dx in range(-3, 4):
            pa = np.zeros((H, W), bool)
            pb = np.zeros((H, W), bool)
            pa[2:2 + a.shape[0], 2:2 + a.shape[1]] = a
            y0, x0 = 2 + dy, 2 + dx
            if y0 < 0 or x0 < 0 or y0 + b.shape[0] > H or x0 + b.shape[1] > W:
                continue
            pb[y0:y0 + b.shape[0], x0:x0 + b.shape[1]] = b
            inter = (pa & pb).sum()
            union = (pa | pb).sum()
            if union:
                best = max(best, inter / union)
    return best


def main():
    table = load_pm_table()
    print("loaded %d poses from generated/shapes_pm.h" % len(table))
    hits = misses = 0
    for shot in sorted(glob.glob(os.path.join(COLOUR_DIR, "s_*_?[0-9].png"))):
        m = re.search(r"s_([0-9A-F]{2})_", os.path.basename(shot))
        if not m:
            continue
        poked = int(m.group(1), 16)
        dump = shot[:-4] + ".bin"
        if not os.path.exists(dump):
            continue
        d = open(dump, "rb").read()
        if len(d) < 0x6120 or d[0x6119] != 0x08:
            continue
        fig = screen_fighter(shot)
        if fig is None or fig.size < 40:
            continue
        scores = sorted(((iou(fig, g), sid) for sid, g in table.items()), reverse=True)
        top, best = scores[0]
        mine = dict((sid, s) for s, sid in scores).get(poked, 0.0)
        ok = best == poked
        hits += ok
        misses += not ok
        print("%s poked=%2d  best=%2d (IoU %.2f)  poked-IoU %.2f  %s"
              % (os.path.basename(shot), poked, best, top, mine, "ok" if ok else "MISMATCH"))
    print("\nmatched %d, mismatched %d" % (hits, misses))
    return 0


if __name__ == "__main__":
    sys.exit(main())
