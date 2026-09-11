#!/bin/bash
# Check the belt rule against the running machine.
#
# $5F66 derives the belt from the score rather than awarding it for anything won. This
# pokes a score into the demo fighter's counter through the emulator's monitor, lets the
# game run a moment so $3C85 recomputes, and reads back $616A -- the belt the ROM itself
# picked. The ten values straddle all five thresholds ($5F59 = $06 $12 $18 $26 $40).
#
# Expect, in order: WHITE YELLOW YELLOW GREEN GREEN PURPLE PURPLE BROWN BROWN BLACK.
# port/hud.h's hudBelt() must agree; verify_fighter.c checks the same ten cases.
#
# Usage: [DISP=:95] probe_belt.sh   -> extracted/beltprobe/bNN.bin, one byte each
set -u
DISP="${DISP:-:95}"
# the repository root, wherever this script happens to live
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$DIR/extracted/beltprobe"; mkdir -p "$OUT"; rm -f "$OUT"/*.bin
FIFO=/tmp/a8belt; rm -f "$FIFO"; mkfifo "$FIFO"
cd "$DIR"
for p in $(pgrep -x atari800 2>/dev/null); do
  tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q "World Karate" && kill -9 "$p" 2>/dev/null
done
SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=software DISPLAY=$DISP \
  atari800 -windowed -no-video-accel -nobasic \
           -horiz-area full -vert-area full -image-aspect none \
           -win-width 768 -win-height 480 \
           "World Karate Championship.atr" < "$FIFO" > /tmp/belt.log 2>&1 &
PID=$!; exec 3>"$FIFO"
sleep 13
W=$(timeout 5 env DISPLAY=$DISP xdotool search --pid $PID 2>/dev/null | tail -1)
[ -n "$W" ] || W=$(timeout 5 env DISPLAY=$DISP xdotool search --onlyvisible --name '.*' 2>/dev/null | tail -1)
timeout 5 env DISPLAY=$DISP xdotool windowfocus "$W" 2>/dev/null
# score pairs to try: F7 F8 for fighter 1 (bytes $FA/$FB, since the demo has $50 = 0)
set -- "00 59" "00 60" "01 19" "01 20" "01 79" "01 80" "02 59" "02 60" "03 99" "04 00"
i=0
for pair in "$@"; do
  i=$((i+1))
  kill -INT $PID; sleep 0.35
  printf 'C FA %s\n' "$pair" >&3; sleep 0.15
  printf 'CONT\n' >&3; sleep 0.35
  kill -INT $PID; sleep 0.35
  printf 'WRITE 616a 616a %s/b%02d.bin\n' "$OUT" $i >&3; sleep 0.35
  printf 'CONT\n' >&3; sleep 0.12
done
kill -INT $PID; sleep 0.3; printf 'QUIT\n' >&3; sleep 0.3
exec 3>&-; kill -9 $PID 2>/dev/null
echo "captured $(ls "$OUT"/*.bin 2>/dev/null | wc -l)"
