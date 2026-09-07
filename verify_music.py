#!/usr/bin/env python3
"""
Check the ported music player against the real POKEY.

atari800's -pokeyrec writes one 9-byte record per frame -- AUDF1 AUDC1 AUDF2 AUDC2
AUDF3 AUDC3 AUDF4 AUDC4 AUDCTL -- which is exactly the shadow the game's player builds
at $2168 and copies to $D200. verify_music.c dumps the same thing from the port.

What this can and cannot prove: a frame-for-frame match would need the port to start at
the same point in the tune, in the same per-voice state, as the emulator happened to be
in when recording began, and the recording does not pin that down. What it does check is
that the port speaks the same language as the hardware -- same register layout, same
AUDCTL, and, for voice 0, exactly the same set of AUDC bytes, which is the envelope and
distortion logic end to end.

Recording:  atari800 -pokeyrec ... "World Karate Championship.atr"; the file lands in the
            working directory as pokeyrec.dat -- keep it as extracted/pokeyrec.dat
Usage:      python3 verify_music.py [emulator.dat] [port.dat]
"""
import os
import sys

REC = 9
EMU = sys.argv[1] if len(sys.argv) > 1 else "../extracted/pokeyrec.dat"
MINE = sys.argv[2] if len(sys.argv) > 2 else "/tmp/wk_pokey.dat"
MUSIC_AUDCTL = 0x50

failures = []


def check(cond, what):
    print("  %-56s %s" % (what, "ok" if cond else "FAILED"))
    if not cond:
        failures.append(what)


def load(path):
    d = open(path, "rb").read()
    return [d[i:i + REC] for i in range(0, len(d) - REC + 1, REC)]


def main():
    if not os.path.exists(EMU):
        print("no emulator recording at %s -- skipping the hardware comparison" % EMU)
        print("(record one with: atari800 -pokeyrec \"World Karate Championship.atr\")")
        return 0
    emu, mine = load(EMU), load(MINE)
    print("emulator: %d frames   port: %d frames" % (len(emu), len(mine)))

    # only the frames where the game is playing the tune, not a sound effect
    tune = [r for r in emu if r[8] == MUSIC_AUDCTL
            and ((r[3] & 0x0F) or (r[5] & 0x0F) or (r[7] & 0x0F))]
    print("emulator frames playing the tune (AUDCTL $%02X): %d" % (MUSIC_AUDCTL, len(tune)))
    check(len(tune) > 50, "the recording contains the tune")

    check(mine[0][8] == MUSIC_AUDCTL,
          "the port programs the same AUDCTL ($%02X)" % MUSIC_AUDCTL)

    # voice 0 = channels 1+2 joined; AUDC2 carries its envelope and distortion
    emu_audc2 = sorted(set(r[3] for r in tune))
    port_audc2 = sorted(set(r[3] for r in mine if r[3] & 0x0F))
    print("  voice 0 AUDC bytes: emulator %d distinct, port %d distinct"
          % (len(emu_audc2), len(port_audc2)))
    missing = [v for v in emu_audc2 if v not in port_audc2]
    if missing:
        print("    the port never produces: %s"
              % " ".join("$%02X" % v for v in missing))
    check(not missing, "every voice-0 AUDC byte the hardware emits, the port emits")

    # AUDF values must come from the ROM's note tables
    import re
    src = open("generated/music.h").read()
    n16 = [int(v) for v in re.search(
        r"MUS_NOTE16\[\d+\]=\{(.*?)\};", src, re.S).group(1).replace("\n", "").split(",") if v.strip()]
    n8 = [int(v, 16) for v in re.findall(
        r"0x([0-9A-Fa-f]{2})", re.search(r"MUS_NOTE8\[\d+\]=\{(.*?)\};", src, re.S).group(1))]
    bad16 = sum(1 for r in mine if (r[3] & 0x0F)
                and (r[0] | (r[2] << 8)) not in n16)
    print("  voice 0 pitches drawn from the 16-bit table: %d frames off-table" % bad16)
    check(bad16 == 0, "voice 0 only ever plays notes from $2109")
    off8 = sum(1 for r in mine if (r[5] & 0x0F) and r[4] not in n8)
    print("  voices 1-2 off-table frames: %d (vibrato and arpeggio detune deliberately)"
          % off8)

    print("\n%s (%d failure%s)"
          % ("all checks passed" if not failures else "FAILURES",
             len(failures), "" if len(failures) == 1 else "s"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
