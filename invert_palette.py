#!/usr/bin/env python3
"""
Recover the per-scanline playfield palette by INVERTING what the real hardware drew,
instead of reverse-engineering the game's chained, self-modifying DLI kernel.

Inputs, both captured in the same frozen state by capture_scenes.sh:
  * a RAM dump -- screen memory at $B000 is 97 lines x 40 bytes of ANTIC mode E
    (160 px per line, 2 bits per pixel)
  * a full-frame screenshot at exactly 2 host px per Atari pixel

For each scanline the pixel VALUES are known from the dump and the pixel COLOURS are
known from the screenshot, so the four active registers (value 0 = COLBK, 1 = COLPF0,
2 = COLPF1, 3 = COLPF2) can simply be read off. The result is correct by construction:
re-rendering the dump through the recovered table must reproduce the screenshot.

Usage: python3 invert_palette.py [dump.bin] [shot.png]
"""
import collections
import sys

from PIL import Image

SCREEN = 0xB000
LINES = 97
BYTES_PER_LINE = 40
PIXELS = BYTES_PER_LINE * 4          # 160 mode-E pixels per line
SCENE_LEFT = 32                      # playfield origin in colour clocks
SCENE_TOP = 31                       # first playfield scanline (display list at $6260)
CLOCKS_PER_PIXEL = 2                 # a mode-E pixel is two colour clocks wide

DUMP = sys.argv[1] if len(sys.argv) > 1 else "extracted/scenes/cur_f3.bin"
SHOT = sys.argv[2] if len(sys.argv) > 2 else "extracted/scenes/cur_f3.png"


def load_frame(path):
    """Screenshot downsampled to one sample per Atari colour clock (384x240)."""
    im = Image.open(path).convert("RGB")
    w, h = im.size
    return im.resize((w // 2, h // 2), Image.NEAREST).load(), w // 2, h // 2


def pixel_values(dump):
    """values[line][p] = the 2-bit mode E pixel value."""
    out = []
    for line in range(LINES):
        base = SCREEN + line * BYTES_PER_LINE
        row = []
        for b in dump[base:base + BYTES_PER_LINE]:
            row += [(b >> 6) & 3, (b >> 4) & 3, (b >> 2) & 3, b & 3]
        out.append(row)
    return out


def score_offset(values, px, W, H, x0, y0):
    """How many lines are colour-consistent at this alignment (higher is better).

    Only values 1..3 are tested. Value 0 selects COLBK, which the game rewrites
    MID-SCANLINE to paint horizontal colour bands, so it is not constant per line and
    would defeat the alignment search."""
    good = 0
    for line in range(LINES):
        y = y0 + line
        if not (0 <= y < H):
            return -1
        seen = {}
        ok = True
        for p in range(PIXELS):
            x = x0 + p * CLOCKS_PER_PIXEL
            if not (0 <= x < W):
                return -1
            v = values[line][p]
            if v == 0:
                continue
            c = px[x, y]
            if v in seen:
                if seen[v] != c:
                    ok = False
                    break
            else:
                seen[v] = c
        if ok:
            good += 1
    return good


def find_alignment(values, px, W, H):
    best = (-1, None)
    for y0 in range(0, 80):
        for x0 in range(0, 96):
            s = score_offset(values, px, W, H, x0, y0)
            if s > best[0]:
                best = (s, (x0, y0))
    return best


def recover(values, px, x0, y0):
    """Per line: constant colours for values 1..3, plus COLBK as x-runs.

    Returns a list of (fixed, bkruns) where fixed maps value 1..3 to a colour and
    bkruns is [(x_start, colour), ...] describing COLBK across the line."""
    table = []
    for line in range(LINES):
        y = y0 + line
        votes = collections.defaultdict(collections.Counter)
        for p in range(PIXELS):
            v = values[line][p]
            if v:
                votes[v][px[x0 + p * CLOCKS_PER_PIXEL, y]] += 1
        fixed = {}
        for v, cnt in votes.items():
            (col, _), = cnt.most_common(1)
            fixed[v] = col
        runs, cur = [], None
        for p in range(PIXELS):
            if values[line][p]:
                continue
            c = px[x0 + p * CLOCKS_PER_PIXEL, y]
            if c != cur:
                runs.append((p, c))
                cur = c
        table.append((fixed, runs))
    return table


def colour_at(entry, value, p):
    """Colour of one mode-E pixel under the recovered model."""
    fixed, runs = entry
    if value:
        return fixed.get(value)
    c = None
    for start, col in runs:
        if start > p:
            break
        c = col
    return c


def render(values, table, path="screenshots/scene_reconstructed.png", scale=2):
    """Re-render the dump through the recovered table -- the proof of correctness."""
    im = Image.new("RGB", (PIXELS * CLOCKS_PER_PIXEL, LINES))
    out = im.load()
    for line in range(LINES):
        for p in range(PIXELS):
            c = colour_at(table[line], values[line][p], p) or (0, 0, 0)
            for k in range(CLOCKS_PER_PIXEL):
                out[p * CLOCKS_PER_PIXEL + k, line] = c
    im = im.resize((im.width * scale, im.height * scale), Image.NEAREST)
    im.save(path)
    return path


def compare(values, table, px, x0, y0):
    """Pixel-exact diff of the reconstruction against the captured screen."""
    bad = 0
    badrows = collections.Counter()
    for line in range(LINES):
        for p in range(PIXELS):
            if colour_at(table[line], values[line][p], p) != px[x0 + p * CLOCKS_PER_PIXEL, y0 + line]:
                bad += 1
                badrows[line] += 1
    return bad, badrows


def emit_header(scenes, path="generated/scenes.h"):
    """Emit every recovered scene as C data.

    scenes: list of (ident, title, bits, table, ground)."""
    import os
    os.makedirs(os.path.dirname(path), exist_ok=True)

    out = []
    out.append("/* scenes.h - background scenes recovered from World Karate Championship.\n"
               " *\n"
               " * Pixel data is the game's own ANTIC mode E screen memory ($B000): %d lines of\n"
               " * %d bytes, 160 pixels per line, 2 bits per pixel. The same bytes live on the\n"
               " * disk at the sectors listed in $449D, and $438C is the routine that loads one.\n"
               " *\n"
               " * Colours were recovered by inverting what the real hardware drew (see\n"
               " * invert_palette.py), not by reverse-engineering the DLI kernel. Values 1..3\n"
               " * are constant per scanline. Value 0 selects COLBK, which the game rewrites\n"
               " * MID-SCANLINE to paint horizontal bands, so it is stored as x-runs.\n"
               " *\n"
               " * Verified: re-rendering through this table reproduces each captured frame\n"
               " * with zero differing pixels.\n"
               " * GENERATED FILE - do not edit; re-run invert_palette.py instead. */\n"
               % (LINES, BYTES_PER_LINE))
    out.append("#ifndef SCENES_H\n#define SCENES_H\n#include <stdint.h>\n")
    out.append("#define SCENE_LINES %d\n#define SCENE_BYTES_PER_LINE %d\n"
               "#define SCENE_PIXELS %d\n" % (LINES, BYTES_PER_LINE, PIXELS))
    out.append("typedef struct { uint8_t r,g,b; } SceneCol;\n")
    out.append("typedef struct { uint8_t x, col; } SceneRun;\n")
    out.append("typedef struct {\n"
               "  const char* name;\n"
               "  SceneCol ground;              /* solid fill below the playfield */\n"
               "  const uint8_t* bits;          /* SCENE_LINES * SCENE_BYTES_PER_LINE */\n"
               "  const uint8_t (*fixed)[3];    /* per line: palette index for values 1,2,3 */\n"
               "  const SceneRun* runs;         /* COLBK runs, concatenated */\n"
               "  const uint16_t* run_ofs;      /* SCENE_LINES+1 offsets into runs */\n"
               "  const SceneCol* pal;\n"
               "  uint16_t pal_n;\n"
               "} BgScene;\n")

    entries = []
    for ident, title, bits, table, ground in scenes:
        pal, idx = [], {}
        def ci(c, pal=pal, idx=idx):
            if c not in idx:
                idx[c] = len(pal)
                pal.append(c)
            return idx[c]
        fixed_idx = [[ci(table[l][0].get(v, (0, 0, 0))) for v in (1, 2, 3)]
                     for l in range(LINES)]
        runs_idx = [[(p, ci(c)) for p, c in table[l][1]] for l in range(LINES)]

        out.append("static const uint8_t %s_bits[%d]={\n" % (ident, len(bits)))
        for i in range(0, len(bits), 20):
            out.append("  " + ",".join("0x%02X" % b for b in bits[i:i + 20]) + ",\n")
        out.append("};\n")
        out.append("static const uint8_t %s_fixed[%d][3]={\n  %s};\n"
                   % (ident, LINES, ",".join("{%d,%d,%d}" % tuple(f) for f in fixed_idx)))
        flat, ofs = [], [0]
        for r in runs_idx:
            flat += r
            ofs.append(len(flat))
        out.append("static const SceneRun %s_runs[%d]={%s};\n"
                   % (ident, max(1, len(flat)),
                      ",".join("{%d,%d}" % (p, c) for p, c in flat) or "{0,0}"))
        out.append("static const uint16_t %s_run_ofs[%d]={%s};\n"
                   % (ident, LINES + 1, ",".join(str(o) for o in ofs)))
        out.append("static const SceneCol %s_pal[%d]={%s};\n"
                   % (ident, len(pal), ",".join("{%d,%d,%d}" % c for c in pal)))
        entries.append('  {"%s", {%d,%d,%d}, %s_bits, %s_fixed, %s_runs, %s_run_ofs,'
                       ' %s_pal, %d}'
                       % (title, ground[0], ground[1], ground[2],
                          ident, ident, ident, ident, ident, len(pal)))
        print("  %-14s %2d palette entries, %4d COLBK runs" % (ident, len(pal), len(flat)))

    out.append("static const BgScene BG_SCENES[]={\n%s,\n};\n" % ",\n".join(entries))
    out.append("#define BG_SCENE_COUNT ((int)(sizeof(BG_SCENES)/sizeof(BG_SCENES[0])))\n")
    out.append("#endif\n")
    open(path, "w").write("".join(out))
    print("wrote %s: %d scenes" % (path, len(scenes)))


# Identified from the captured frames; these are International Karate's stages.
NAMES = {
    0: "SYDNEY",
    1: "NEW YORK",
    2: "RIO DE JANEIRO",
    3: "JAPAN",
    4: "EGYPT",
    5: "GREECE",
    6: "MOUNT FUJI",
}


def process(dump_path, shot_path):
    """Recover one scene; returns (bits, table, ground) or None if not exact."""
    dump = open(dump_path, "rb").read()
    px, W, H = load_frame(shot_path)
    values = pixel_values(dump)
    score, off = find_alignment(values, px, W, H)
    if off is None:
        return None
    x0, y0 = off
    table = recover(values, px, x0, y0)
    bad, _ = compare(values, table, px, x0, y0)
    nruns = sum(len(r) for _, r in table)
    print("  align x0=%d y0=%d, %d/%d lines consistent, %d COLBK runs, %d px differ"
          % (x0, y0, score, LINES, nruns, bad))
    if bad:
        return None
    ground = px[x0 + 4, y0 + LINES + 6]
    bits = dump[SCREEN:SCREEN + LINES * BYTES_PER_LINE]
    return values, bits, table, ground


def main():
    import glob
    import os
    scenes, skipped = [], []
    for i in range(7):
        shots = sorted(glob.glob("extracted/scenes/scene%d_f*.png" % i))
        got = None
        for shot in shots:
            dump = shot[:-4] + ".bin"
            if not os.path.exists(dump):
                continue
            print("scene %d <- %s" % (i, os.path.basename(shot)))
            got = process(dump, shot)
            if got:
                break
        if not got:
            skipped.append(i)
            continue
        values, bits, table, ground = got
        if i == 0:
            render(values, table)
        scenes.append(("scene%d" % i, NAMES.get(i, "SCENE %d" % i), bits, table, ground))
    if skipped:
        print("NOT recovered exactly, omitted: %s" % skipped)
    if scenes:
        emit_header(scenes)


if __name__ == "__main__":
    main()
