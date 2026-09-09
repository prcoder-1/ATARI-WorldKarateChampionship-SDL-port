#!/usr/bin/env python3
"""
Emit the tables the original's hit test runs on, straight from a RAM dump.

The test is `$415D`, called once per game tick from the fight loop (`$288A: JSR $2729`).
It is not a sprite overlap. It works on points:

  * A blow can only be thrown on a tick where the attacker's **drawn shape** is one of
    eight listed in `$400B`, and only if that pose has not been held for two ticks or
    more (`$6185 >= 2`).
  * The attacker gets one strike point, its own x plus `$4014[strike]`, mirrored inside
    the $24-wide box when it faces left.
  * The defender's shape maps through `$4082` to a target class -- `>= $80` means it
    cannot be hit at all -- and the class gives six body parts through `$40BB`, each an
    offset from the defender's x, again `>= $80` for a part it does not have.
  * Each strike reaches only some parts: `$405C[part] AND $401C[strike]`.
  * The distance between the two points is formed one of four ways, by the two fighters'
    facings (`$6138 AND 3`), and compared against a near and a far limit per strike.
    Under the near limit the blow is solid and scores two; between near and far it is a
    glancing one and scores one; beyond the far limit, or negative, it misses and the
    next part is tried.

  $400B  strike shapes, 8 of them and a $FF sentinel
  $4014  the strike point's offset from the attacker's x
  $401C  which body parts this strike can reach
  $4024  the far limit          $402C  the near limit
  $4034  score for a solid blow    $403C  score for a glancing one (BCD, added at $4550)
  $4044  announcement for a solid blow   $404C  for a glancing one ($613F)
  $4054  a column bias by facing pair    $405C  one bit per body part
  $4062  the move forced on the defender, by strike and facing pair
  $4082  defender shape -> target class
  $40BB  class * 6 + part -> the part's offset from the defender's x

Usage: python3 extract_hit.py [dump.bin]
"""
import glob
import os
import sys

NSHAPES = 54
NSTRIKE = 9          # eight strikes and the $FF sentinel the search never matches
NPARTS = 6
OUT = "generated/hit.h"

STRIKE_SHAPE = 0x400B
STRIKE_POINT = 0x4014
STRIKE_PARTS = 0x401C
FAR_LIMIT = 0x4024
NEAR_LIMIT = 0x402C
SCORE_SOLID = 0x4034
SCORE_GLANCE = 0x403C
SIGN_SOLID = 0x4044
SIGN_GLANCE = 0x404C
COLUMN_BIAS = 0x4054
PART_BIT = 0x405C
REACTION = 0x4062
SHAPE_CLASS = 0x4082
PART_OFFSET = 0x40BB


def find_dump():
    if len(sys.argv) > 1:
        return sys.argv[1]
    for pat in ("../extracted/ram_true_64k.bin", "../extracted/fightdumps/*.bin",
                "../extracted/scenes/*.bin"):
        got = sorted(glob.glob(pat))
        if got:
            return got[0]
    raise SystemExit("no RAM dump found")


def main():
    path = find_dump()
    d = open(path, "rb").read()
    if len(d) < 0x4200:
        raise SystemExit("%s is too short to hold the hit tables" % path)

    cls = [d[SHAPE_CLASS + i] for i in range(NSHAPES)]
    live = [c for c in cls if c < 0x80]
    # $41AC promotes class $18 to $1A when the strike is number 3, so the class table has
    # to be read one entry past its largest ordinary value.
    nclass = max(max(live) if live else 0, 0x1A) + 1

    print("%s" % os.path.basename(path))
    print("  strike shapes: %s" % [d[STRIKE_SHAPE + i] for i in range(NSTRIKE)])
    print("  %d target classes, %d parts each" % (nclass, NPARTS))
    absent = sum(1 for c in range(nclass) for p in range(NPARTS)
                 if d[PART_OFFSET + c * NPARTS + p] >= 0x80)
    print("  %d of %d class/part slots are marked absent"
          % (absent, nclass * NPARTS))
    unhittable = sum(1 for c in cls if c >= 0x80)
    print("  %d of %d shapes cannot be hit at all" % (unhittable, NSHAPES))

    out = []
    out.append("/* hit.h - the tables the original's hit test runs on.\n"
               " *\n"
               " * Read out of a RAM dump by extract_hit.py; the routine that consumes them is\n"
               " * hit_test.h, transcribed from $415D. The test is a geometric one between a\n"
               " * strike point on the attacker and one of six body points on the defender --\n"
               " * not an overlap of the two sprites.\n"
               " * GENERATED FILE - do not edit. */\n")
    out.append("#ifndef HIT_H\n#define HIT_H\n#include <stdint.h>\n")
    out.append("#define HIT_STRIKES %d\n#define HIT_PARTS %d\n#define HIT_CLASSES %d\n"
               % (NSTRIKE, NPARTS, nclass))
    out.append("#define HIT_SHAPES %d\n" % NSHAPES)
    out.append("/* a table entry of $80 or more means \"absent\": $419D and $4223 both\n"
               " * branch on the sign bit rather than comparing against a sentinel */\n")
    out.append("#define HIT_ABSENT 0x80\n")
    out.append("/* $41A2: class $18 becomes $1A when the strike is number 3 */\n")
    out.append("#define HIT_CLASS_SPECIAL 0x18\n#define HIT_CLASS_SPECIAL_TO 0x1A\n"
               "#define HIT_CLASS_SPECIAL_STRIKE 3\n")
    out.append("/* $41BB: class $13 collapses to 0 when the situation code is 0 or 7 */\n")
    out.append("#define HIT_CLASS_FLAT 0x13\n")
    out.append("/* $4185: a pose held this long or longer cannot strike */\n")
    out.append("#define HIT_HOLD_LIMIT 2\n")
    out.append("/* $42EC/$42F2: the announcement column is clamped to these */\n")
    out.append("#define HIT_COL_MIN 0x04\n#define HIT_COL_MAX 0x27\n")

    def arr(name, addr, n, comment=None):
        if comment:
            out.append("/* %s */\n" % comment)
        out.append("static const uint8_t %s[%d]={%s};\n"
                   % (name, n, ",".join("0x%02X" % d[addr + i] for i in range(n))))

    arr("HIT_STRIKE_SHAPE", STRIKE_SHAPE, NSTRIKE, "$400B shape -> is this a strike")
    arr("HIT_STRIKE_POINT", STRIKE_POINT, NSTRIKE, "$4014 the strike point's offset")
    arr("HIT_STRIKE_PARTS", STRIKE_PARTS, NSTRIKE, "$401C parts this strike reaches")
    arr("HIT_FAR", FAR_LIMIT, NSTRIKE, "$4024 the far limit")
    arr("HIT_NEAR", NEAR_LIMIT, NSTRIKE, "$402C the near limit")
    arr("HIT_SCORE_SOLID", SCORE_SOLID, NSTRIKE, "$4034 BCD score, blow under the near limit")
    arr("HIT_SCORE_GLANCE", SCORE_GLANCE, NSTRIKE, "$403C BCD score, blow between the limits")
    arr("HIT_SIGN_SOLID", SIGN_SOLID, NSTRIKE, "$4044 $613F for a solid blow")
    arr("HIT_SIGN_GLANCE", SIGN_GLANCE, NSTRIKE, "$404C $613F for a glancing one")
    arr("HIT_COL_BIAS", COLUMN_BIAS, 4, "$4054 column bias by facing pair")
    arr("HIT_PART_BIT", PART_BIT, NPARTS, "$405C one bit per body part")
    out.append("/* $4062 the move forced on the defender: [strike*4 + facing pair] */\n")
    out.append("static const uint8_t HIT_REACTION[%d]={%s};\n"
               % (NSTRIKE * 4,
                  ",".join("0x%02X" % d[REACTION + i] for i in range(NSTRIKE * 4))))
    arr("HIT_SHAPE_CLASS", SHAPE_CLASS, NSHAPES, "$4082 defender shape -> target class")
    out.append("/* $40BB the six body points of each class, as offsets from the\n"
               " * defender's x; HIT_ABSENT or more means the class has no such part */\n")
    out.append("static const uint8_t HIT_PART_OFFSET[HIT_CLASSES*HIT_PARTS]={\n")
    for c in range(nclass):
        out.append("  %s,\n" % ",".join("0x%02X" % d[PART_OFFSET + c * NPARTS + p]
                                        for p in range(NPARTS)))
    out.append("};\n#endif\n")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").write("".join(out))
    print("wrote %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
