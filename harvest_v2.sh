#!/bin/bash
# Definitive shape harvest.
#
# Improvements over harvest_shapes.sh / screen_harvest.sh:
#   * -horiz-area full -vert-area full  -> the WHOLE 384x240 frame is visible, so the
#     fighter is never clipped at the feet (the old 800x600 window showed a crop).
#   * -win 768x480 with -image-aspect none -> exactly 2 host px per Atari pixel, so the
#     screenshot downsamples to the sprite grid by integer division, no resampling.
#   * captures the composited SCREEN *and* a RAM dump ($0000-$7FFF) in the same frozen
#     state, so screen pixels and machine state can be correlated.
#   * background reference is taken in the same frozen state, first, with both fighters
#     parked on unused shape 4.
#
# Usage: harvest_v2.sh [outdir]   (default ../extracted/harvest2)
set -u
# DISP: X display to drive. Xvfb :99 is shared and has been seen to wedge; use a
# private display by default and start it if it is not up.
DISP="${DISP:-:95}"
DIR="/home/prcoder/claude-experiments/WorldKarate"
OUT="${1:-$DIR/extracted/harvest2}"
mkdir -p "$OUT"
# KEEP=1 appends to an existing harvest (used to top up shapes that never landed
# on the usable double-buffer parity); default wipes.
[ "${KEEP:-0}" = "1" ] || rm -f "$OUT"/*.png "$OUT"/*.bin
FIFO=/tmp/a8v2; rm -f "$FIFO"; mkfifo "$FIFO"
cd "$DIR"

# Reap leftovers from an aborted run: a stale instance keeps its window around and the
# window lookup below can latch onto it. Only ever touch OUR emulator -- an unrelated
# atari800 may legitimately be running.
for p in $(pgrep -x atari800 2>/dev/null); do
  if tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null | grep -q "World Karate"; then
    kill -9 "$p" 2>/dev/null
  fi
done

# Bring up our own X display if needed, and fail loudly if it does not answer.
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
           "World Karate Championship.atr" < "$FIFO" > /tmp/hv2.log 2>&1 &
PID=$!
exec 3>"$FIFO"

# Boot, then let the attract-mode bout start. The emulator must be driven into the
# monitor soon after this: left free-running it terminates roughly 15s post-boot, and
# emulation is suspended whenever the monitor has control, which is why the capture
# loop below (short CONT bursts) can then run for minutes.
sleep 13
# Bind to OUR emulator's window: an unrelated atari800 may be running, and a bare
# `xdotool search ""` can latch onto the wrong window.
# NOTE: `xdotool search --pid N --onlyvisible` with no name pattern BLOCKS FOREVER.
# Use --pid on its own, under a timeout. Only needed when screenshots are taken.
W=$(timeout 5 env DISPLAY=$DISP xdotool search --pid $PID 2>/dev/null | tail -1)
# Fall back to the newest visible window: --pid matching depends on the SDL window
# carrying _NET_WM_PID, which it does not always do.
[ -n "$W" ] || W=$(timeout 5 env DISPLAY=$DISP xdotool search --onlyvisible --name '.*' 2>/dev/null | tail -1)
[ -n "$W" ] || { echo "ERROR: emulator window not found (alive? $(kill -0 $PID 2>/dev/null && echo yes || echo no))"; kill -9 $PID 2>/dev/null; exit 1; }
timeout 5 env DISPLAY=$DISP xdotool windowfocus "$W" 2>/dev/null
timeout 5 env DISPLAY=$DISP xdotool key F2 2>/dev/null; sleep 2
timeout 5 env DISPLAY=$DISP xdotool key F2 2>/dev/null; sleep 2

# Freeze both frame->shape stores so a poked shape id survives.
kill -INT $PID; sleep 0.7
printf 'C 5423 EA EA EA\n' >&3; sleep 0.15
printf 'C 54FC EA EA EA\n' >&3; sleep 0.15
# Also freeze horizontal movement: $5509 applies FRAME_VELX to $E0/$E1 every frame, so
# without this the fighters drift between the poke and the screenshot and the x window
# derived from the dump no longer lines up with the captured pixels.
printf 'C 5509 60\n' >&3; sleep 0.15
printf 'CONT\n' >&3; sleep 0.2

# The game DOUBLE-BUFFERS the P/M area: $6119 alternates $08 (players at $0C00-$0FFF,
# valid) and $18 (the other state, where the buffer holds stale garbage). A capture is
# only usable when it lands on $6119=$08, so each shape is attempted several times and
# the valid attempt is selected offline by pick_shapes.py.
#
# $1 = hex shape id for fighter B ("04" = parked/unused), $2 = output basename
# Set BOTH fighters to the shape under test, and push them apart.
#
# Parking one fighter on unused shape 4 does NOT blank it -- the P/M buffer keeps its
# last pose -- so "the occupied band" was ambiguous and the first extraction came out
# mislabelled (check_shape_ids.py). Driving both fighters to the same shape removes the
# ambiguity: every occupied band, and both on-screen figures, are the shape under test.
# $E0/$E1 are the fighters' X positions (valid range $10..$AE). They are placed well
# inside the arena and 72 units apart: at the old $24/$8C the red fighter sat against the
# right edge of the playfield and its wider poses were clipped by the screen, and the
# extraction window derived from $5384 cut them again on the left.
cap() {
  kill -INT $PID; sleep 0.30
  printf 'C DD %s\n' "$1" >&3
  printf 'C DE %s\n' "$1" >&3
  printf 'C E0 30\n' >&3
  printf 'C E1 78\n' >&3
  sleep 0.12
  printf 'CONT\n' >&3; sleep 0.18          # rendered, minimal drift
  # NOSHOT=1 skips the screenshot: the sprite itself comes from the RAM dump, and
  # `import` can block indefinitely if the window is unmapped.
  [ "${NOSHOT:-0}" = "1" ] || timeout 5 env DISPLAY=$DISP import -window "$W" "$OUT/$2.png" 2>/dev/null
  kill -INT $PID; sleep 0.30
  printf 'WRITE 0000 7fff %s/%s.bin\n' "$OUT" "$2" >&3; sleep 0.40
  printf 'CONT\n' >&3; sleep 0.08
}

ATTEMPTS=${ATTEMPTS:-3}
TAG=${TAG:-a}                      # distinguishes attempts from separate runs
SHAPES=${SHAPES:-$(seq 0 47)}      # decimal shape ids to harvest
# Always capture a parked-fighter reference for THIS run: colour-sprite extraction
# subtracts it from the shape frames, so it must come from the same boot.
for a in $(seq 1 $ATTEMPTS); do cap 04 "bg_${TAG}$a"; done
for i in $SHAPES; do
  hx=$(printf '%02X' $i)
  kill -0 $PID 2>/dev/null || { echo "emulator died before shape $hx; stopping"; break; }
  echo "shape $hx"
  for a in $(seq 1 $ATTEMPTS); do cap "$hx" "$(printf 's_%02X_%s%d' $i "$TAG" $a)"; done
done

kill -INT $PID; sleep 0.4
printf 'QUIT\n' >&3; sleep 0.4
exec 3>&-; kill -9 $PID 2>/dev/null
echo "DONE -> $OUT"
ls "$OUT"/*.png 2>/dev/null | wc -l
