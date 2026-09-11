#!/bin/bash
# Capture background scenes: full RAM ($0000-$FFFF, so $B000 screen memory and the $6400
# charset are both included) plus the composited screen, in the same frozen state.
#
# Shares the hard-won environment rules with harvest_v2.sh:
#   * private X display (the shared :99 has been seen to wedge)
#   * `xdotool search --pid N --onlyvisible` with no name pattern BLOCKS FOREVER
#   * this atari800 quits by itself ~15s after boot if left free-running; emulation is
#     suspended while the monitor has control, so break in early and run in short bursts
#   * full-frame geometry: exactly 2 host px per Atari pixel, nothing cropped
#
# Both fighters are parked on unused shape 4 so the playfield is unobstructed.
#
# Env: DISP (default :95), FRAMES (consecutive frames per scene, default 3),
#      SCENES (values to poke into $5C; empty = just capture the current scene)
set -u
DISP="${DISP:-:95}"
# the repository root, wherever this script happens to live
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${1:-$DIR/extracted/scenes}"
FRAMES=${FRAMES:-3}
SCENES=${SCENES:-}
mkdir -p "$OUT"; rm -f "$OUT"/*.png "$OUT"/*.bin
FIFO=/tmp/a8scn; rm -f "$FIFO"; mkfifo "$FIFO"
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
           "World Karate Championship.atr" < "$FIFO" > /tmp/scn.log 2>&1 &
PID=$!
exec 3>"$FIFO"
sleep 13
W=$(timeout 5 env DISPLAY=$DISP xdotool search --pid $PID 2>/dev/null | tail -1)
[ -n "$W" ] || { echo "ERROR: emulator window not found"; kill -9 $PID 2>/dev/null; exit 1; }
timeout 5 env DISPLAY=$DISP xdotool windowfocus "$W" 2>/dev/null
timeout 5 env DISPLAY=$DISP xdotool key F2 2>/dev/null; sleep 2
timeout 5 env DISPLAY=$DISP xdotool key F2 2>/dev/null; sleep 2

# Freeze the frame->shape stores so the parked shape id sticks.
kill -INT $PID; sleep 0.7
printf 'C 5423 EA EA EA\n' >&3; sleep 0.15
printf 'C 54FC EA EA EA\n' >&3; sleep 0.15
printf 'CONT\n' >&3; sleep 0.2

# $1 = output basename
grab() {
  kill -INT $PID; sleep 0.30
  printf 'C DD 04\n' >&3            # park both fighters: clean playfield
  printf 'C DE 04\n' >&3
  sleep 0.12
  printf 'CONT\n' >&3; sleep 0.18
  timeout 5 env DISPLAY=$DISP import -window "$W" "$OUT/$1.png" 2>/dev/null
  kill -INT $PID; sleep 0.30
  printf 'WRITE 0000 ffff %s/%s.bin\n' "$OUT" "$1" >&3; sleep 0.60
  printf 'CONT\n' >&3; sleep 0.08
}

if [ -z "$SCENES" ]; then
  for f in $(seq 1 $FRAMES); do grab "$(printf 'cur_f%d' $f)"; done
else
  # $613D is the level index (masked & 7); $449D holds each level's start sector, and
  # $438C is the game's own "advance to the next level" routine: it takes $5C, bumps it
  # modulo 7 into $613D, reads 4096 bytes off the disk and copies $0400-$13FF to the
  # $B000 screen. Setting $5C to target-1 and running $438C to completion through the
  # monitor's `R` command loads any scene on demand, without playing through the game.
  for s in $SCENES; do
    kill -0 $PID 2>/dev/null || { echo "emulator died before scene $s"; break; }
    prev=$(( (s + 6) % 7 ))
    echo "scene $s (via \$5C=$prev then R 438C)"
    kill -INT $PID; sleep 0.30
    printf 'C 5C %02X\n' "$prev" >&3; sleep 0.15
    printf 'R 438C\n' >&3; sleep 2.5           # runs the level load, incl. disk I/O
    printf 'CONT\n' >&3; sleep 0.40            # let the DLI colours settle
    for f in $(seq 1 $FRAMES); do grab "$(printf 'scene%d_f%d' $s $f)"; done
  done
fi

kill -INT $PID; sleep 0.4
printf 'QUIT\n' >&3; sleep 0.4
exec 3>&-; kill -9 $PID 2>/dev/null
echo "DONE -> $OUT"
ls "$OUT"/*.bin 2>/dev/null | wc -l
