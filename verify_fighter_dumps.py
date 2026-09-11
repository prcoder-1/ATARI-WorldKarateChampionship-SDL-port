#!/usr/bin/env python3
"""
Check the fighter state machine's invariants against the running machine.

verify_fighter.c exercises the ported state machine on its own terms. This one goes to
the other end: it reads RAM dumps taken while the real game was fighting and asserts the
things the port relies on, so that a misreading of $53BC shows up as a failure here
rather than as odd-looking motion on screen.

The invariant that matters most is the $0050,y gate. $53D6 and $543F each test it and
jump over their whole block when it is zero, so a CPU-controlled fighter never unwinds a
pose backwards ($6187), never stalls on a held frame ($6185), and never re-dispatches
partway through a move. The port used to run all three paths for both fighters, which
broke the CPU opponent's movement up.

  $0050,y  nonzero = joystick, zero = the CPU routine
  $6181,y  move id            $00EE,y  animation frame index
  $6185,y  hold counter       $6187,y  reverse flag
  $00DD,y  shape id           $00E0,y  x, left edge

Usage: python3 verify_fighter_dumps.py [dump ...]
"""
import glob
import os
import sys

MOVE_START = 0x558D
FRAME_SHAPE = 0x55B1
SHAPE_W = 0x5384
ARENA_LEFT, ARENA_RIGHT = 0x10, 0xAE

failures = []
checks = 0


def check(cond, what, detail=""):
    global checks
    checks += 1
    print("  %-56s %s" % (what, "ok" if cond else "FAILED"))
    if detail:
        print("      " + detail)
    if not cond:
        failures.append(what)


def main():
    files = sys.argv[1:] or sorted(glob.glob("extracted/fightdumps/dump_*.bin"))
    files = [f for f in files if os.path.getsize(f) >= 0x6200]
    if not files:
        print("no fight dumps present -- harvest them first; skipping")
        return 0

    dumps = [open(f, "rb").read() for f in files]
    print("%d RAM dumps taken during a fight" % len(dumps))

    cpu = hum = cpu_rev = cpu_hold = 0
    bad_frame = bad_shape = bad_x = 0
    moves = set()
    for d in dumps:
        for y in (0, 1):
            human = d[0x50 + y]
            move = d[0x6181 + y]
            frame = d[0x00EE + y]
            shape = d[0x00DD + y]
            x = d[0x00E0 + y]
            moves.add(move)
            if human:
                hum += 1
            else:
                cpu += 1
                cpu_rev += d[0x6187 + y] != 0
                cpu_hold += d[0x6185 + y] != 0
            lo, hi = d[MOVE_START + move], d[MOVE_START + move + 1]
            if not lo <= frame < hi:
                bad_frame += 1
            elif d[FRAME_SHAPE + frame] != shape:
                bad_shape += 1
            w = d[SHAPE_W + shape]
            left = x + 0x24 - w if d[0x00E2 + y] else x
            if left < ARENA_LEFT - 1 or left + w > ARENA_RIGHT + 1:
                bad_x += 1

    print("  %d CPU-controlled samples, %d joystick-controlled, %d distinct moves"
          % (cpu, hum, len(moves)))

    check(cpu > 0, "the dumps contain CPU-controlled fighters")
    check(cpu_rev == 0,
          "$53D6: a CPU fighter is never in reverse playback",
          "%d of %d samples had $6187 set" % (cpu_rev, cpu) if cpu_rev else "")
    check(cpu_hold == 0,
          "$543F: a CPU fighter never holds a frame",
          "%d of %d samples had $6185 set" % (cpu_hold, cpu) if cpu_hold else "")
    check(bad_frame == 0,
          "$540A: the frame index stays inside its move's range",
          "%d samples outside" % bad_frame if bad_frame else "")
    check(bad_shape == 0,
          "$5418: the drawn shape is FRAME_SHAPE of the frame index",
          "%d samples disagreed" % bad_shape if bad_shape else "")
    check(bad_x == 0,
          "$553E/$5564: the fighter stays inside the arena",
          "%d samples outside $10..$AE" % bad_x if bad_x else "")

    print("\n%s (%d of %d checks failed)"
          % ("FAILURES" if failures else "all checks passed", len(failures), checks))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
