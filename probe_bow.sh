#!/bin/bash
# Capture a real bow, to settle where the game actually draws its two poses.
#
# shapes_pm.h has shape 48 at x0 = -32 while the stand at x0 = -4 carries a bitmap that
# is identical to it byte for byte, which cannot both be right. The harvest that measured
# it poked $00DD to force a shape the game was not otherwise in; this instead makes the
# game bow of its own accord and watches.
#
# $2D89 is the reset that opens a bout, and $2DC0 inside it hands both fighters move $1C.
# Running it through the monitor starts a real bow, driven by the state machine, with
# every register the game itself would have set.
#
# Frames are grabbed with their RAM, so measuring is the same arithmetic
# extract_sprites.py does: x0 = (the ink's left edge, in colour clocks) - (2*$E0 + 28).
#
# Env: DISP (default :93), FRAMES (default 14, one bow)
set -u
DISP="${DISP:-:93}"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$DIR/extracted/bow}"
FRAMES=${FRAMES:-14}
mkdir -p "$OUT"; rm -f "$OUT"/*.png "$OUT"/*.bin
FIFO=/tmp/a8bow; rm -f "$FIFO"; mkfifo "$FIFO"
cd "$DIR"

for p in $(pgrep -x atari800 2>/dev/null); do
  if tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q "World Karate"; then
    kill -9 "$p" 2>/dev/null
  fi
done

if ! timeout 5 env DISPLAY=$DISP xdpyinfo >/dev/null 2>&1; then
  Xvfb "$DISP" -screen 0 800x600x24 >/dev/null 2>&1 &
  sleep 2
fi
timeout 5 env DISPLAY=$DISP xdpyinfo >/dev/null 2>&1 || {
  echo "ERROR: X display $DISP is not responding"; exit 1; }

SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=software DISPLAY=$DISP \
  atari800 -windowed -no-video-accel -nobasic \
           -horiz-area full -vert-area full -image-aspect none \
           -win-width 768 -win-height 480 \
           "World Karate Championship.atr" < "$FIFO" > /tmp/bow.log 2>&1 &
PID=$!
exec 3>"$FIFO"
sleep 13
W=$(timeout 5 env DISPLAY=$DISP xdotool search --pid $PID 2>/dev/null | tail -1)
[ -n "$W" ] || { echo "ERROR: emulator window not found"; kill -9 $PID 2>/dev/null; exit 1; }

# Open a bout: $2D89 resets both fighters, and $2DC0 in it starts the bow.
kill -INT $PID; sleep 0.7
printf 'C D0 01\n' >&3; sleep 0.15          # an ordinary bout, so the machine runs
# FREEZE=1 pins the animation on the frame the bow starts from, which is the only way to
# get a still of its upright pose: the monitor cannot stop the machine finely enough to
# land on a particular tick, and every unfrozen grab caught the bent one. $53FF and $54E9
# are the two stores that advance $00EE,Y; with them out, $5423 still derives $00DD,Y from
# the frame, so the shape and the placement are the game's own and not poked.
if [ "${FREEZE:-0}" = 1 ]; then
  printf 'C 53FF EA EA EA\n' >&3; sleep 0.15
  printf 'C 54E9 EA EA EA\n' >&3; sleep 0.15
fi
printf 'R 2D89\n' >&3;  sleep 0.8
printf 'CONT\n' >&3;    sleep 0.10

# The screen and the RAM have to come from the SAME instant or the arithmetic is
# meaningless: the fighters move between a running grab and a monitor dump. So the
# emulator is stopped first and both are taken while it is held there.
# The monitor's R resumes the machine when the subroutine returns, so the delay between
# starting a bow and stopping again is what decides how far into it we look. The bow is
# 14 ticks -- five upright, five bent, four upright -- at about ten a second, so walking
# that delay from almost nothing up to a second samples all three stretches. Left at a
# fixed 0.4s every grab caught frame 157, the middle of the bent stretch.
for f in $(seq 1 $FRAMES); do
  n=$(printf 'bow_%02d' $f)
  [ "${FREEZE:-0}" = 1 ] || printf 'R 2D89\n' >&3
  sleep "0.$(printf '%02d' $(( (f * 7) % 100 )))"
  kill -INT $PID; sleep 0.35                   # freeze the screen and the RAM together
  timeout 5 env DISPLAY=$DISP import -window "$W" "$OUT/$n.png" || echo "import failed for $n"
  printf 'WRITE 0000 ffff %s/%s.bin\n' "$OUT" "$n" >&3; sleep 0.45
done

kill -INT $PID; sleep 0.4
printf 'QUIT\n' >&3; sleep 0.4
exec 3>&-; kill -9 $PID 2>/dev/null
echo "DONE -> $OUT"
ls "$OUT"/*.bin 2>/dev/null | wc -l
