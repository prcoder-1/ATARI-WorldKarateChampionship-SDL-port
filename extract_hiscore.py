#!/usr/bin/env python3
"""
Emit the high-score table: its text, its layout and its starting contents.

The table lives in RAM as seven parallel arrays of seven slots -- six entries and a
candidate in slot 6 -- and $60B1 bubble-sorts all seven by score, which is how a new
score finds its place:

    60D3: LDA $6220,X / CMP $6220,Y / BCC .. / BNE ..    ; the score, high byte
    60DD: LDA $6227,X / CMP $6227,Y                      ; then the low
    60E4: swap seven fields, seven bytes apart

    $6220 score high   $6227 score low   $622E "this is the new one" marker
    $6235 belt         $623C/$6243/$624A the three characters of the name

It is drawn as TEXT in the ground region, not on a screen of its own. $5DBC copies the
HUD's character set from $6400 into the lower character sets at character $19, so a
game character code c becomes c + $19 down there; the text is stored as ASCII and
converted with SBC #$1D, which is the same thing ($36 to reach a game code, $19 back).

    5FE5: LDA $5FC8,Y / SBC #$1D / STA $08CA,Y   ; the header: row 4, column 10, 28 chars
    5FFC: $62 = $092A                            ; the rows: row 6 on, column 10
    6017: LDA $D6 / ADC #$1A -> +1               ; the position digit
    6022: $623C,X $6243,X $624A,X -> +6,+7,+8    ; the name
    6045: JSR $5ED6                              ; the belt, six characters at +11
    6050: $6220,X $6227,X and a zero -> +20      ; six score digits, leading zeros blanked

Everything below is read out of a dump. The port draws it with the same font it draws
the HUD with, so the codes are emitted in HUD form (the ROM's lower-charset codes minus
$19).

Usage: python3 extract_hiscore.py [dump.bin]
"""
import os
import sys

HEADER = 0x5FC8         # ASCII, converted with SBC #$1D
HEADER_LEN = 0x1C       # $5FF4: CPY #$1C
BELT_TEXT = 0x5EFF
BELT_SHORT_OFFSETS = 0x5EF7
NBELTS = 6
BELT_SHORT_LEN = 6      # $5EE6/$5EF2: Y from $0B to $10

SCORE_HI = 0x6220
SCORE_LO = 0x6227
NEW_MARK = 0x622E
BELT = 0x6235
NAME = (0x623C, 0x6243, 0x624A)
ENTRIES = 6

# where it lands in the ground text region
HEADER_ROW = 4          # $08CA - $0800 = $12A/$C0 with stride $30
ROW0 = 6                # $092A
COL = 10
OFF_POS = 1
OFF_NAME = 6
OFF_BELT = 11
OFF_SCORE = 20
ASCII_BIAS = 0x36       # ASCII -> game character code
LOWER_BIAS = 0x19       # game character code -> the lower character set's code

OUT = "generated/hiscore.h"
DUMPS = ("../extracted/ram_true_64k.bin", "../extracted/fightdumps/dump_00.bin")


def find_dump():
    if len(sys.argv) > 1:
        return sys.argv[1]
    for p in DUMPS:
        if os.path.exists(p) and os.path.getsize(p) > 0x6260:
            return p
    return None


def show(codes):
    out = ""
    for c in codes:
        c &= 0x7F
        if c <= 0x09:
            out += chr(48 + c)
        elif c == 0x0A:
            out += " "
        elif 0x0B <= c <= 0x24:
            out += chr(65 + c - 0x0B)
        else:
            out += "?"
    return out


def main():
    path = find_dump()
    if path is None:
        # not in this repository -- see README.md
        print("no RAM dump holding the table -- skipping")
        return 0
    d = open(path, "rb").read()

    header = [(d[HEADER + i] - ASCII_BIAS) & 0xFF for i in range(HEADER_LEN)]
    belts = []
    for b in range(NBELTS):
        o = d[BELT_SHORT_OFFSETS + b]
        belts.append([(d[BELT_TEXT + o + k] - ASCII_BIAS) & 0xFF
                      for k in range(BELT_SHORT_LEN)])
    table = []
    for e in range(ENTRIES):
        table.append((d[SCORE_HI + e], d[SCORE_LO + e], d[BELT + e],
                      [(d[n + e] - LOWER_BIAS) & 0xFF for n in NAME]))

    print("%s" % os.path.basename(path))
    print("  header (%d chars): %r" % (HEADER_LEN, show(header)))
    for b, name in enumerate(belts):
        print("  belt %d: %r" % (b, show(name)))
    print("  the table as it starts:")
    for e, (hi, lo, belt, name) in enumerate(table):
        print("    %d: score $%02X%02X00  belt %d  name %r"
              % (e + 1, hi, lo, belt, show(name)))

    out = []
    out.append("/* hiscore.h - the high-score table: its text, layout and starting rows.\n"
               " *\n"
               " * Seven parallel arrays of seven slots in the ROM, six entries and a\n"
               " * candidate, bubble-sorted by score ($60B1/$60D3/$60E4). Drawn as text in\n"
               " * the ground region with the HUD's own character set, which $5DBC copies\n"
               " * into the lower character sets at code $19 -- so these codes are the HUD's,\n"
               " * with that bias taken back off.\n"
               " * GENERATED FILE - do not edit; re-run extract_hiscore.py instead. */\n")
    out.append("#ifndef HISCORE_H\n#define HISCORE_H\n#include <stdint.h>\n")
    out.append("#define HS_ENTRIES %d\n#define HS_NAME_LEN %d\n"
               % (ENTRIES, len(NAME)))
    out.append("#define HS_HEADER_LEN %d\n#define HS_BELT_LEN %d\n"
               % (HEADER_LEN, BELT_SHORT_LEN))
    out.append("/* rows and columns of the ground text region ($08CA, $092A) */\n")
    out.append("#define HS_HEADER_ROW %d\n#define HS_ROW0 %d\n#define HS_COL %d\n"
               % (HEADER_ROW, ROW0, COL))
    out.append("#define HS_OFF_POS %d\n#define HS_OFF_NAME %d\n"
               "#define HS_OFF_BELT %d\n#define HS_OFF_SCORE %d\n"
               % (OFF_POS, OFF_NAME, OFF_BELT, OFF_SCORE))
    out.append("static const uint8_t HS_HEADER[HS_HEADER_LEN]={%s};\n"
               % ",".join("0x%02X" % c for c in header))
    out.append("/* the six-character belt names $5ED6 draws, not the twelve-character\n"
               " * ones the HUD uses */\n")
    out.append("static const uint8_t HS_BELT[%d][HS_BELT_LEN]={\n%s};\n"
               % (NBELTS, "".join("  {%s},\n" % ",".join("0x%02X" % c for c in b)
                                  for b in belts)))
    out.append("/* the table as the game starts with it */\n")
    out.append("typedef struct { uint8_t hi, lo, belt, name[HS_NAME_LEN]; } HsEntry;\n")
    out.append("static const HsEntry HS_START[HS_ENTRIES]={\n")
    for hi, lo, belt, name in table:
        out.append("  {0x%02X,0x%02X,0x%02X,{%s}},\n"
                   % (hi, lo, belt, ",".join("0x%02X" % c for c in name)))
    out.append("};\n#endif\n")
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").write("".join(out))
    print("wrote %s" % OUT)
    return 0


if __name__ == "__main__":
    sys.exit(main())
