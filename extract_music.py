#!/usr/bin/env python3
"""
Emit the game's background music as C data, straight from a RAM dump.

The player is a three-voice tracker (see SPRITE_AND_MUSIC_INTERNALS.md part 2):
sequence -> pattern -> note or command, with a per-voice envelope, vibrato and
arpeggio, writing a nine-byte shadow that is copied to $D200-$D208.

  $21D0/$21D3  sequence pointers, one per voice (terminated by $FF)
  $21D6/$21F1  pattern pointers (terminated by $FF)
  $20D2        note-length table, indexed by event & $1F
  $20DB        note -> AUDF, 8-bit, for voices 1 and 2
  $2109        note -> AUDF, 16-bit words, for voice 0 (POKEY channels 1+2 joined)
  $217A/$217F  envelope pointers
  $2171        arpeggio table ($1F terminates and wraps)
  $2170        AUDCTL

Usage: python3 extract_music.py [dump.bin]
"""
import glob
import os
import sys

SEQ_LO, SEQ_HI = 0x21D0, 0x21D3
PAT_LO, PAT_HI = 0x21D6, 0x21F1
NPATTERNS = 27
DUR = 0x20D2
DUR_N = 9
NOTE8 = 0x20DB
NOTE8_N = 46
NOTE16 = 0x2109
NOTE16_N = 32
ENV_LO, ENV_HI = 0x217A, 0x217F
NENV = 5
ARP = 0x2171
ARP_N = 9
SHADOW = 0x2168
OUT = "generated/music.h"

# per-voice state the sequence data does not reset (only $214A..$2155 is cleared
# on restart), so the values a running game holds are the starting point
INSTR, AUDCBASE, VIBDEP, ARPPOS, ARPON = 0x2156, 0x2159, 0x215C, 0x2160, 0x2163


def find_dump():
    if len(sys.argv) > 1:
        return sys.argv[1]
    for pat in ("extracted/scenes/*.bin", "extracted/colour4/*.bin"):
        got = sorted(glob.glob(pat))
        if got:
            return got[0]
    return None


def read_until_ff(d, addr, limit=512):
    out = []
    while len(out) < limit:
        out.append(d[addr + len(out)])
        if out[-1] == 0xFF:
            break
    return out


def main():
    path = find_dump()
    if path is None:
        # not in this repository -- see README.md
        print("no RAM dump holding the music -- skipping")
        return 0
    d = open(path, "rb").read()

    seqs = [read_until_ff(d, d[SEQ_LO + v] | (d[SEQ_HI + v] << 8)) for v in range(3)]
    pats = [read_until_ff(d, d[PAT_LO + p] | (d[PAT_HI + p] << 8))
            for p in range(NPATTERNS)]
    envs = [read_until_ff(d, d[ENV_LO + e] | (d[ENV_HI + e] << 8), 128)
            for e in range(NENV)]

    print("%s: sequences %s, %d patterns (%d bytes), %d envelopes"
          % (os.path.basename(path), [len(s) for s in seqs], NPATTERNS,
             sum(len(p) for p in pats), NENV))

    out = []
    out.append("/* music.h - the game's background music, as data.\n"
               " *\n"
               " * Read out of a RAM dump by extract_music.py; the player that consumes it is\n"
               " * pokey.h, ported from $1F06. Voice 0 drives POKEY channels 1+2 joined into one\n"
               " * 16-bit channel at the 1.79 MHz clock; voices 1 and 2 drive channels 3 and 4\n"
               " * at 64 kHz. AUDCTL says so.\n"
               " * GENERATED FILE - do not edit. */\n")
    out.append("#ifndef MUSIC_H\n#define MUSIC_H\n#include <stdint.h>\n")
    out.append("#define MUS_VOICES 3\n#define MUS_PATTERNS %d\n#define MUS_ENVELOPES %d\n"
               % (NPATTERNS, NENV))
    out.append("#define MUS_AUDCTL 0x%02X\n" % d[SHADOW + 8])
    out.append("/* the player runs from the vertical blank, skipping one tick in twelve\n"
               " * ($1F06: DEC $2166 / BPL, reloading with 10) */\n")
    out.append("#define MUS_DIVIDER_RELOAD 0x0A\n")

    def arr(name, vals, per=24, ctype="uint8_t"):
        out.append("static const %s %s[%d]={\n" % (ctype, name, len(vals)))
        for k in range(0, len(vals), per):
            out.append("  " + ",".join(("0x%02X" % v) if ctype == "uint8_t" else str(v)
                                       for v in vals[k:k + per]) + ",\n")
        out.append("};\n")

    for v in range(3):
        arr("mus_seq%d" % v, seqs[v])
    out.append("static const uint8_t* const MUS_SEQ[MUS_VOICES]={%s};\n"
               % ",".join("mus_seq%d" % v for v in range(3)))

    for p in range(NPATTERNS):
        arr("mus_pat%02d" % p, pats[p])
    out.append("static const uint8_t* const MUS_PAT[MUS_PATTERNS]={\n  %s};\n"
               % ",".join("mus_pat%02d" % p for p in range(NPATTERNS)))

    for e in range(NENV):
        arr("mus_env%d" % e, envs[e])
    out.append("static const uint8_t* const MUS_ENV[MUS_ENVELOPES]={%s};\n"
               % ",".join("mus_env%d" % e for e in range(NENV)))

    arr("MUS_DUR", [d[DUR + k] for k in range(DUR_N)])
    arr("MUS_NOTE8", [d[NOTE8 + k] for k in range(NOTE8_N)])
    arr("MUS_NOTE16", [d[NOTE16 + 2 * k] | (d[NOTE16 + 2 * k + 1] << 8)
                       for k in range(NOTE16_N)], per=16, ctype="uint16_t")
    arr("MUS_ARP", [d[ARP + k] for k in range(ARP_N)])

    out.append("/* per-voice state the restart does not clear */\n")
    arr("MUS_INIT_INSTR", [d[INSTR + v] for v in range(3)])
    arr("MUS_INIT_AUDC", [d[AUDCBASE + v] for v in range(3)])
    arr("MUS_INIT_VIBDEPTH", [d[VIBDEP + v] for v in range(3)])
    arr("MUS_INIT_ARPPOS", [d[ARPPOS + v] for v in range(3)])
    arr("MUS_INIT_ARPON", [d[ARPON + v] for v in range(3)])
    out.append("#endif\n")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").write("".join(out))
    print("wrote %s" % OUT)


if __name__ == "__main__":
    main()
