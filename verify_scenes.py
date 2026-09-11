#!/usr/bin/env python3
"""
Check the port's C background renderer against the model recovered from the emulator.

verify_scenes.c writes /tmp/cscene<N>.ppm by calling sceneDraw() -- the same code the
game uses. This script independently recomputes each scene from its RAM dump plus its
screen capture and compares pixel by pixel. Zero differences means the chain

    emulator capture  ==  recovered model  ==  the port's renderer

holds for every scene.
"""
import os
import sys

import invert_palette as ip

SCENES = 7


def c_render(i):
    f = open("/tmp/cscene%d.ppm" % i, "rb")
    assert f.readline().strip() == b"P6"
    w, _h = map(int, f.readline().split())
    f.readline()
    return f.read(), w


def main():
    total = 0
    for i in range(SCENES):
        shot = "../extracted/scenes/scene%d_f1.png" % i
        dump = shot[:-4] + ".bin"
        if not (os.path.exists(shot) and os.path.exists(dump)):
            # The game's own data is not in this repository; capture_scenes.sh makes
            # these. See README.md.
            print("no scene captures present -- run capture_scenes.sh first; skipping")
            return 0
        px, W, H = ip.load_frame(shot)
        values = ip.pixel_values(open(dump, "rb").read())
        _score, off = ip.find_alignment(values, px, W, H)
        x0, y0 = off
        table = ip.recover(values, px, x0, y0)
        data, w = c_render(i)
        bad = 0
        for line in range(ip.LINES):
            for p in range(ip.PIXELS):
                want = ip.colour_at(table[line], values[line][p], p)
                o = ((ip.SCENE_TOP + line) * w + ip.SCENE_LEFT + p * 2) * 3
                if (data[o], data[o + 1], data[o + 2]) != want:
                    bad += 1
        print("scene %d (%-14s): %d differing pixels"
              % (i, ip.NAMES.get(i, "?"), bad))
        total += bad
    print("TOTAL: %d differing pixels" % total)
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main())
