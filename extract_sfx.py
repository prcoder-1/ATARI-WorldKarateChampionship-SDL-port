#!/usr/bin/env python3
"""
Emit the game's sound effects as C data, straight from a RAM dump.

They are **digitised samples**, not tones. $3A7C starts one: it points a 16-bit cursor
at the sample data, sets AUDF1 = $10, zeroes the rest of POKEY (so AUDCTL = 0, all
channels on the 64 kHz clock) and enables the POKEY timer-1 interrupt. The handler at
$3AE0 then fires at 63921/(AUDF1+1) = about 3760 Hz, reads one byte, takes a nibble and
writes it to AUDC1, AUDC2 and AUDC3 with bit 4 set -- POKEY's volume-only mode -- so the
nibble is the speaker level. The cursor stops when its high byte reaches the effect's
end page.

Which nibble is chosen is patched into the handler at load time ($3B1C-$3B1F): four
`LSR A` for the high nibble, or `AND #$0F` for the low one. That is why the six effects
occupy only three byte ranges -- each range carries two effects, one per nibble.

Tables:
  $3A64  end page per effect          $3A6A  start address per effect (16-bit)
  $3A76  bit 7 set = high nibble
  $39D3  the 11 shape ids that trigger a sound when a fighter changes to them
  $39DF  effect while fighting        $39EA  effect while the referee has play frozen
  $39F5  delay, and $3A00 the effect to play after it
  $3A0B  effect in game states 2 and 5

Usage: python3 extract_sfx.py [dump.bin]
"""
import glob
import os
import sys

SFX_LO, SFX_HI = 0x9F00, 0xB000        # the sample data
T_ENDPAGE, T_START, T_NIBBLE = 0x3A64, 0x3A6A, 0x3A76
NEFFECTS = 6
T_SHAPE, T_NORMAL, T_FROZEN = 0x39D3, 0x39DF, 0x39EA
T_DELAY, T_DELAYED, T_STATE25 = 0x39F5, 0x3A00, 0x3A0B
NEVENTS = 11
ATTR_MASK = 0x4D                       # $3A34: LDA $6178 / AND #$4D
AUDF1 = 0x10                           # $3AB8
POKEY_CLK = 63921.0
OUT = "generated/sfx.h"


def find_dumps():
    got = sorted(glob.glob("extracted/scenes/*.bin"))
    if not got:
        return None
    return got


def main():
    dumps = sys.argv[1:] or find_dumps()
    d = open(dumps[0], "rb").read()
    if len(d) < SFX_HI:
        raise SystemExit("%s does not reach $%04X" % (dumps[0], SFX_HI))

    # the samples must be resident, not overlaid by the level loader
    for other in dumps[1:4]:
        o = open(other, "rb").read()
        if len(o) >= SFX_HI and o[SFX_LO:SFX_HI] != d[SFX_LO:SFX_HI]:
            print("WARNING: sample data differs in %s" % os.path.basename(other))

    data = d[SFX_LO:SFX_HI]
    starts = [d[T_START + 2 * i] | (d[T_START + 2 * i + 1] << 8) for i in range(NEFFECTS)]
    endpg = [d[T_ENDPAGE + i] for i in range(NEFFECTS)]
    nib = [d[T_NIBBLE + i] for i in range(NEFFECTS)]
    rate = POKEY_CLK / (AUDF1 + 1)

    print("%s: %d bytes of sample data at $%04X-$%04X, %.0f Hz"
          % (os.path.basename(dumps[0]), len(data), SFX_LO, SFX_HI - 1, rate))
    for i in range(NEFFECTS):
        n = (endpg[i] << 8) - starts[i]
        print("  effect %d: $%04X..$%02XFF  %4d samples, %s nibble, %.2f s"
              % (i, starts[i], endpg[i] - 1, n,
                 "high" if nib[i] & 0x80 else "low", n / rate))

    out = []
    out.append("/* sfx.h - the game's sound effects: 4-bit digitised samples.\n"
               " *\n"
               " * Read out of a RAM dump by extract_sfx.py. Played back by $3AE0 from a POKEY\n"
               " * timer-1 interrupt at %.0f Hz, one nibble per interrupt, written to AUDC1-3\n"
               " * in volume-only mode. Three byte ranges carry six effects: each range is read\n"
               " * once for its high nibbles and once for its low ones.\n"
               " * GENERATED FILE - do not edit. */\n" % rate)
    out.append("#ifndef SFX_H_DATA\n#define SFX_H_DATA\n#include <stdint.h>\n")
    out.append("#define SFX_COUNT %d\n#define SFX_EVENTS %d\n" % (NEFFECTS, NEVENTS))
    out.append("#define SFX_BASE 0x%04X\n" % SFX_LO)
    out.append("#define SFX_RATE %.4f          /* 63921 / (AUDF1 + 1), AUDF1 = $%02X */\n"
               % (rate, AUDF1))
    out.append("/* $3A34: a sound only fires while fighting if the frame's attribute\n"
               " * bits pass this mask */\n")
    out.append("#define SFX_ATTR_MASK 0x%02X\n" % ATTR_MASK)

    def arr(name, vals, ctype="uint8_t", per=16, fmt="0x%02X"):
        out.append("static const %s %s[%d]={\n" % (ctype, name, len(vals)))
        for k in range(0, len(vals), per):
            out.append("  " + ",".join(fmt % v for v in vals[k:k + per]) + ",\n")
        out.append("};\n")

    arr("SFX_DATA", data, per=20)
    arr("SFX_START", [s - SFX_LO for s in starts], ctype="uint16_t", fmt="%d")
    arr("SFX_END", [(e << 8) - SFX_LO for e in endpg], ctype="uint16_t", fmt="%d")
    arr("SFX_HIGH_NIBBLE", [1 if n & 0x80 else 0 for n in nib], fmt="%d")

    out.append("/* the shape ids whose appearance triggers a sound ($39D3) */\n")
    arr("SFX_EVENT_SHAPE", [d[T_SHAPE + i] for i in range(NEVENTS)], fmt="%d")
    for name, addr in (("SFX_WHILE_FIGHTING", T_NORMAL), ("SFX_WHILE_FROZEN", T_FROZEN),
                       ("SFX_DELAY", T_DELAY), ("SFX_DELAYED", T_DELAYED),
                       ("SFX_STATE_2_5", T_STATE25)):
        out.append("/* $%04X */\n" % addr)
        arr(name, [d[addr + i] for i in range(NEVENTS)], fmt="%d")
    out.append("#endif\n")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "w").write("".join(out))
    print("wrote %s" % OUT)


if __name__ == "__main__":
    main()
