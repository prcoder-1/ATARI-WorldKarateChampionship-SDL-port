#!/usr/bin/env python3
"""
Emit the game's own logic tables as C, straight from a RAM dump.

game_data.h previously carried these typed out by hand, truncated to 21 moves and 120
animation frames. The real tables are longer: $558D stays monotonic for 36 entries, so
there are 35 moves spanning 199 frames. Everything below is read from the dump instead,
so the sizes cannot drift again.

  $558D  move -> first animation frame (move m uses [S[m], S[m+1]))
  $55B1  frame -> shape id
  $5679  frame -> signed horizontal velocity
  $5740  frame -> attribute bits
  $5384  shape -> width in sprite pixels
  $6BC0  shape -> height of the segment-encoded source data
  $528F/$52AF/$52CF/$52EF  stick+fire -> move, by facing, base and alternate
  $5287  situation code -> may the alternate tables be used

Usage: python3 extract_tables.py [dump.bin]
"""
import glob
import os
import sys

MOVE_START = 0x558D
FRAME_SHAPE = 0x55B1
FRAME_VELX = 0x5679
FRAME_ATTR = 0x5740
SHAPE_W = 0x5384
SHAPE_H = 0x6BC0
DISPATCH = {"DISPATCH_R": 0x528F, "DISPATCH_L": 0x52AF,
            "DISPATCH_altR": 0x52CF, "DISPATCH_altL": 0x52EF}
MOVE_GATE = 0x5287
# --- CPU opponent ($3D04) ---
AI_TABLES = [
    ("AI_RND_CROUCH", 0x3E42, 6,  "skill $F6 -- RANDOM gate for the anti-crouch reply"),
    ("AI_RND_IDLE",   0x3E48, 6,  "skill $F6 -- RANDOM gate for the idle/far branch"),
    ("AI_RND_CLOSE",  0x3E4E, 6,  "skill $F6 -- RANDOM gate for the close-range branch"),
    ("AI_COLS",       0x3E54, 6,  "skill $F6 -- columns of AI_BY_RANGE to choose from"),
    ("AI_SIT_A",      0x3E5A, 8,  "situation code $6138 -- picks the AI_FAR/AI_NEAR pair"),
    ("AI_SIT_B",      0x3E6A, 8,  "situation code $6138 -- picks the reply-to-move branch"),
    ("AI_MOVE_FLAG",  0x3E72, 35, "opponent move id -- $FF defer, else picks the reply table"),
    ("AI_REPLY0",     0x3E94, 8,  "reply when AI_MOVE_FLAG[opponent move] == 0"),
    ("AI_REPLY1",     0x3E9C, 8,  "reply when it is nonzero"),
    ("AI_FAR",        0x3EA4, 12, "gap >= 8"),
    ("AI_NEAR",       0x3EB0, 12, "gap < 8"),
    ("AI_MID",        0x3EBC, 12, "gap >= 10 on the other branch"),
    ("AI_ANTI_CROUCH", 0x3EC8, 32, "(orientation << 3) | RANDOM&7"),
    ("AI_BY_RANGE",   0x3EE8, 120, "12 columns per row; row = AI_ROW[$6136]"),
    ("AI_ROW",        0x3F66, 11, "gap in quarters $6136 -> row offset into AI_BY_RANGE"),
]
NSHAPES = 54
OUT = "generated/frames.h"
TIMING_OUT = "generated/timing.h"


def find_dump():
    if len(sys.argv) > 1:
        return sys.argv[1]
    for pat in ("../extracted/scenes/*.bin", "../extracted/colour4/*.bin"):
        got = sorted(glob.glob(pat))
        if got:
            return got[0]
    raise SystemExit("no RAM dump found")


def main():
    path = find_dump()
    d = open(path, "rb").read()

    starts = []
    i = 0
    while True:
        v = d[MOVE_START + i]
        if i and v < starts[-1]:
            break
        starts.append(v)
        i += 1
    nmoves = len(starts) - 1
    nframes = starts[-1]
    print("%s: %d moves, %d animation frames" % (os.path.basename(path), nmoves, nframes))

    def sgn(b):
        return b - 256 if b >= 128 else b

    out = []
    out.append("/* frames.h - the game's own animation and dispatch tables.\n"
               " *\n"
               " * Read directly out of a RAM dump by extract_tables.py; see that file for the\n"
               " * addresses. A fighter's state is (move, frame): the frame index walks\n"
               " * [MOVE_FRAME_START[m], MOVE_FRAME_START[m+1]) and these tables say what\n"
               " * happens on each frame.\n"
               " * GENERATED FILE - do not edit. */\n")
    out.append("#ifndef FRAMES_H\n#define FRAMES_H\n#include <stdint.h>\n")
    out.append("#define NUM_MOVE_IDS %d\n#define NUM_FRAMES %d\n#define NUM_SHAPES %d\n"
               % (nmoves, nframes, NSHAPES))

    def arr(ctype, name, vals, per=20):
        out.append("static const %s %s[%d]={\n" % (ctype, name, len(vals)))
        for k in range(0, len(vals), per):
            out.append("  " + ",".join(str(v) for v in vals[k:k + per]) + ",\n")
        out.append("};\n")

    arr("uint8_t", "MOVE_FRAME_START", starts)
    arr("uint8_t", "FRAME_SHAPE", [d[FRAME_SHAPE + k] for k in range(nframes)])
    arr("int8_t", "FRAME_VELX", [sgn(d[FRAME_VELX + k]) for k in range(nframes)])
    arr("uint8_t", "FRAME_ATTR", [d[FRAME_ATTR + k] for k in range(nframes)])
    arr("uint8_t", "SHAPE_WIDTH", [d[SHAPE_W + k] for k in range(NSHAPES)])
    arr("uint8_t", "SHAPE_HEIGHT", [d[SHAPE_H + k] for k in range(NSHAPES)])
    for name, addr in DISPATCH.items():
        arr("uint8_t", name, [d[addr + k] for k in range(32)], per=32)
    arr("uint8_t", "MOVE_GATE", [d[MOVE_GATE + k] for k in range(8)], per=8)

    out.append("\n/* --- the CPU opponent's tables ($3D04) --- */\n")
    for name, addr, n, what in AI_TABLES:
        out.append("/* $%04X: %s */\n" % (addr, what))
        arr("uint8_t", name, [d[addr + k] for k in range(n)], per=16)

    out.append("#endif\n")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").write("".join(out))
    print("wrote %s" % OUT)

    hi = max(d[FRAME_SHAPE + k] for k in range(nframes))
    print("highest shape id referenced by any frame: %d" % hi)
    emit_timing(d)


def emit_timing(d):
    """The bout's timing constants, all counted in video frames."""
    t = []
    t.append("/* timing.h - the original's bout timings, in video frames.\n"
             " *\n"
             " * $00DF is a frame counter bumped by the vertical blank; the bout code at\n"
             " * $2F5F..$3029 spins on it, so these comparisons ARE the durations.\n"
             " *\n"
             " * The round length is not a clock: $6154 counts REFEREE TRAVERSALS. The\n"
             " * referee's x ($6159) starts at $28 or $DC and steps by $615A each frame\n"
             " * ($58AD, normally 2) until it passes $F0 or falls under $0A, and each turn\n"
             " * decrements $6154 ($5834). $2F3A gives the number of traversals per round.\n"
             " * GENERATED FILE - do not edit. */\n")
    t.append("#ifndef TIMING_H\n#define TIMING_H\n#include <stdint.h>\n")
    t.append("/* $2F9A: hold before the fighters are shown */\n")
    t.append("#define T_READY   0x%02X\n" % 0x50)
    t.append("/* $2FA9: hold until the bout is enabled ($615D) */\n")
    t.append("#define T_BEGIN   0x%02X\n" % 0xC8)
    t.append("/* $2FEA and $3017: the freeze after a point, and after time runs out */\n")
    t.append("#define T_FREEZE  0x%02X\n" % 0x80)
    t.append("/* $2F7C: both fighters are placed at this x when a bout starts */\n")
    t.append("#define T_START_X 0x%02X\n" % 0x54)
    t.append("/* $2F3A, indexed by the round number $615F */\n")
    t.append("static const uint8_t ROUND_TRAVERSALS[3]={%s};\n"
             % ",".join(str(d[0x2F3A + k]) for k in range(3)))
    t.append("/* The referee does NOT move. $6159 is how far through one of his signalling\n"
             " * actions he is: $58EF starts one, setting $6159 to $28 or $DC and $615A to a\n"
             " * step from $58AD, and $5807 walks it to $F0 or $0A. Reaching the end is what\n"
             " * decrements the round counter $6154 ($5834), so a round is a number of his\n"
             " * actions, not of anything geometric. $58A1 picks which of three actions, by\n"
             " * round number and a random draw. */\n")
    t.append("static const uint8_t REF_ACTION[12]={%s};   /* $58A1 */\n"
             % ",".join(str(d[0x58A1 + k]) for k in range(12)))
    t.append("static const uint8_t REF_STEP[12]={%s};     /* $58AD */\n"
             % ",".join(str(d[0x58AD + k]) for k in range(12)))
    t.append("static const uint8_t REF_SIDE[12]={%s};     /* $5885 */\n"
             % ",".join(str(d[0x5885 + k]) for k in range(12)))
    t.append("#define REF_END_LOW   0x0A\n#define REF_END_HIGH  0xF0\n")
    t.append("#define REF_START_UP   0x28\n#define REF_START_DOWN 0xDC\n")
    t.append("/* $3BF9: the main loop only takes a game tick once the vertical blank has\n"
             " * counted more than this many frames into $6121, so the fighters advance at\n"
             " * one tick per divider+1 video frames -- 10 Hz at the default setting, not 60.\n"
             " * $619B selects the entry; a hidden key sequence changes it ($33C3). */\n")
    t.append("static const uint8_t GAME_TICK_DIVIDER[4]={%s};\n"
             % ",".join(str(d[0x3BF5 + k]) for k in range(4)))
    t.append("#define GAME_TICK_DEFAULT %d\n" % d[0x619B])
    t.append("/* $3C01: while the referee has play frozen ($00D8) the divider is this */\n")
    t.append("#define GAME_TICK_FROZEN 6\n")
    t.append("/* $00DC: the clock the HUD shows. BCD, decremented once every $3C frames\n"
             " * by $3988, and started at $30 for one player or $60 for two. */\n")
    t.append("#define T_CLOCK_1P 0x%02X\n#define T_CLOCK_2P 0x%02X\n#define T_CLOCK_TICK 0x%02X\n"
             % (0x30, 0x60, 0x3C))
    t.append("#endif\n")
    open(TIMING_OUT, "w").write("".join(t))
    print("wrote %s: rounds %s traversals, referee step %d"
          % (TIMING_OUT, [d[0x2F3A + k] for k in range(3)], d[0x58AD]))


if __name__ == "__main__":
    main()
