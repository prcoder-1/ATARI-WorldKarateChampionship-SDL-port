#!/bin/bash
# Capture the referee's announcement signs from the running game.
#
# The signs are not decodable: the wording is nowhere in RAM or on the disk, in ASCII or
# in the game's own character codes, and the $0800 character memory that covers the
# ground is blank while a sign is showing. So they are photographed instead, from the
# attract-mode demo, which plays a whole bout and puts up BEGIN, HALF POINT, FULL POINT,
# RED, WHITE and a bonus figure on its own.
#
# extract_signs.py then collects the distinct ones. Run this more than once with
# different TAG values to widen the coverage -- each run sees whatever the demo happens
# to do.
#
# Usage: [TAG=d] [SHOTS=160] [DISP=:95] harvest_signs.sh
set -u
DISP="${DISP:-:95}"
# the repository root, wherever this script happens to live
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$DIR/extracted/signs"; mkdir -p "$OUT"; :
FIFO=/tmp/a8sig; rm -f "$FIFO"; mkfifo "$FIFO"
cd "$DIR"
for p in $(pgrep -x atari800 2>/dev/null); do
  tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q "World Karate" && kill -9 "$p" 2>/dev/null
done
SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=software DISPLAY=$DISP \
  atari800 -windowed -no-video-accel -nobasic \
           -horiz-area full -vert-area full -image-aspect none \
           -win-width 768 -win-height 480 \
           "World Karate Championship.atr" < "$FIFO" > /tmp/sig.log 2>&1 &
PID=$!; exec 3>"$FIFO"
sleep 13
W=$(timeout 5 env DISPLAY=$DISP xdotool search --pid $PID 2>/dev/null | tail -1)
[ -n "$W" ] || W=$(timeout 5 env DISPLAY=$DISP xdotool search --onlyvisible --name '.*' 2>/dev/null | tail -1)
[ -n "$W" ] || { echo "no window"; kill -9 $PID; exit 1; }
timeout 5 env DISPLAY=$DISP xdotool windowfocus "$W" 2>/dev/null
# Drive the demo in short bursts: this build quits ~15s after boot if left free-running,
# but time stops while the monitor holds it.
for i in $(seq 1 ${SHOTS:-140}); do
  kill -INT $PID; sleep 0.18
  printf 'CONT\n' >&3; sleep 0.30
  timeout 5 env DISPLAY=$DISP import -window "$W" "$OUT/${TAG:-d}$(printf %03d $i).png" 2>/dev/null
  kill -0 $PID 2>/dev/null || { echo "emulator gone at $i"; break; }
done
kill -INT $PID; sleep 0.3; printf 'QUIT\n' >&3; sleep 0.3
exec 3>&-; kill -9 $PID 2>/dev/null
echo "captured $(ls "$OUT"/*.png 2>/dev/null | wc -l) frames"
