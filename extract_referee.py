#!/usr/bin/env python3
"""
Emit the referee as C data.

The referee is a third figure, drawn from the same Player/Missile hardware as the two
fighters but in its own scanline band, above them and below the playfield. He paces the
arena, and each turn he makes takes one off the round counter ($6154) -- so he is the
round clock as well as scenery. $5807 moves him:

    $6159  his x, stepping by $615A ($58AD, normally 2)
    $615B  direction; he turns at $F0 going right and $0A going left
    $6154  decremented on each turn, at $5834

He is captured rather than decoded: his graphics do not come from the fighters' shape
table, and $595A only draws the small pointer arrows that go with him. Frames where both
fighters are on a standing pose leave his band clear, so he isolates cleanly there.

Usage: python3 extract_referee.py [capture_dir ...]
"""
import glob
import os
import struct
import sys
import zlib

import numpy as np
from PIL import Image

CAPTURES = sys.argv[1:] or ["../extracted/colour4", "../extracted/scenes"]
GI_WHITE = (211, 211, 211)
SKIN = (189, 113, 121)
OUTLINE = (4, 4, 4)
IDX_GI, IDX_SKIN, IDX_OUTLINE = 1, 2, 3
BAND_TOP, BAND_BOT = 130, 172      # his band: below the playfield, above the fighters
CLOCKS_PER_PX = 2
X_ORIGIN = 28                      # the same mapping the fighters use: clock = 2*x + 28
OUT = "generated/referee.h"


def frame(path):
    return np.array(Image.open(path).convert("RGB")).astype(int)[::2, ::2]


def eq(im, c):
    return (im[:, :, 0] == c[0]) & (im[:, :, 1] == c[1]) & (im[:, :, 2] == c[2])


def isolate(im):
    """The referee as a colour-index grid, plus his top scanline and left clock."""
    gi, skin, out = eq(im, GI_WHITE), eq(im, SKIN), eq(im, OUTLINE)
    band = np.zeros(gi.shape, bool)
    band[BAND_TOP:BAND_BOT, :] = True
    coloured = (gi | skin) & band
    if not coloured.any():
        return None
    ys, xs = np.nonzero(coloured)
    y0, y1, x0, x1 = ys.min(), ys.max(), xs.min(), xs.max()
    if (x1 - x0) > 60 or (y1 - y0) > 40:
        return None                      # merged with the sign or a fighter
    ink = (gi | skin | out) & band
    while y0 - 1 >= BAND_TOP and ink[y0 - 1, x0:x1 + 1].any():
        y0 -= 1
    while y1 + 1 < BAND_BOT and ink[y1 + 1, x0:x1 + 1].any():
        y1 += 1
    w = (x1 - x0) // CLOCKS_PER_PX + 1
    g = np.zeros((y1 - y0 + 1, w), np.uint8)
    for y in range(y0, y1 + 1):
        for k in range(w):
            x = x0 + k * CLOCKS_PER_PX
            if gi[y, x]:
                g[y - y0, k] = IDX_GI
            elif skin[y, x]:
                g[y - y0, k] = IDX_SKIN
            elif out[y, x]:
                g[y - y0, k] = IDX_OUTLINE
    return g, y0, x0


def main():
    seen = {}
    files = []
    for d in CAPTURES:
        files += sorted(glob.glob(os.path.join(d, "*.png")))
    for p in files:
        got = isolate(frame(p))
        if got is None:
            continue
        g, y0, x0 = got
        key = (g.shape, g.tobytes())
        seen.setdefault(key, []).append((g, y0, x0, os.path.basename(p)))
    if not seen:
        print("the referee was not found in any capture")
        return 1

    # the pose the most frames agree on
    best = max(seen.values(), key=len)
    g, y0, x0, name = best[0]
    print("referee: %dx%d, top scanline %d, agreed by %d of %d frames (%s)"
          % (g.shape[1], g.shape[0], y0, len(best), len(files), name))
    print("  %d distinct appearances in all; the rest are him mid-stride or with a sign"
          % len(seen))
    for r in g:
        print("    " + "".join(" .oO"[v] for v in r))

    out = []
    out.append("/* referee.h - the referee, captured from the screen.\n"
               " *\n"
               " * A third figure on the same Player/Missile hardware as the fighters, in his\n"
               " * own scanline band. He paces the arena and each turn takes one off the round\n"
               " * counter $6154, so he is the round clock as well ($5807).\n"
               " * GENERATED FILE - do not edit; re-run extract_referee.py instead. */\n")
    out.append("#ifndef REFEREE_H_DATA\n#define REFEREE_H_DATA\n#include <stdint.h>\n")
    out.append("#define REF_W %d\n#define REF_H %d\n#define REF_Y %d\n"
               % (g.shape[1], g.shape[0], y0))
    out.append("#define REF_CLOCKS_PER_PX %d\n#define REF_X_ORIGIN %d\n"
               % (CLOCKS_PER_PX, X_ORIGIN))
    out.append("/* colour indices: 1 = gi, 2 = skin, 3 = outline */\n")
    out.append("static const uint8_t REF_PX[REF_H*REF_W]={\n")
    for r in g:
        out.append("  " + ",".join(str(int(v)) for v in r) + ",\n")
    out.append("};\n#endif\n")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").write("".join(out))
    print("wrote %s" % OUT)

    pal = {0: (12, 12, 20), IDX_GI: GI_WHITE, IDX_SKIN: SKIN, IDX_OUTLINE: (0, 0, 0)}
    W, H, sc = g.shape[1], g.shape[0], 6
    pix = [pal[0]] * (W * H)
    for y in range(H):
        for x in range(W):
            pix[y * W + x] = pal[g[y, x]]
    _png("screenshots/referee.png", pix, W, H, sc)
    print("wrote screenshots/referee.png")
    return 0


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
    sys.exit(main())
