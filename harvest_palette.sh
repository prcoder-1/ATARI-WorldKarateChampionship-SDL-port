#!/bin/bash
# Measure the machine's colour palette: what RGB each of the 256 GTIA colour bytes
# actually comes out as, on the same emulator every other colour in the port was taken
# from. The port needs it because the belt colours are stored as colour bytes ($5F60) and
# the black belt's is computed at run time ($14 & $F0 | $0A), so there is no capture to
# read them off -- see extract_palette.py.
#
# The trick is to stop being the game. VVBLKI ($0222) points at $3901; patching that to
# jump into a routine that never returns hands the machine over for good. The routine
# turns off NMIs and playfield DMA, so the whole screen is COLBK, then walks COLBK down
# the frame one scanline at a time from a settable start:
#
#   0600: A9 00     LDA #$00
#   0602: 8D 0E D4  STA NMIEN     ; no more DLIs or VBIs
#   0605: 8D 00 D4  STA DMACTL    ; blank the playfield: the screen is COLBK alone
#   060B: AD 0B D4  LDA VCOUNT    ; line up with the top of the frame, so a scanline's
#   060E: D0 FB     BNE $060B     ;   colour is a known function of its position
#   0610: A2 xx     LDX #START    ; <- $0611 is patched between passes
#   0612: A0 F0     LDY #240
#   0614: 8E 0A D4  STX WSYNC
#   0617: 8E 1A D0  STX COLBK
#   061A: E8        INX
#   061B: 88        DEY
#   061C: D0 F6     BNE $0614
#   061E: 4C 0B 06  JMP $060B
#
# 240 scanlines are visible and there are 256 colours, so it takes two passes: one from
# $00 and one from $80. Between them every byte lands on screen, and the values they
# share are what lets extract_palette.py check one pass against the other.
#
# The environment rules are capture_scenes.sh's, learned the hard way: private X display,
# `xdotool search --pid N --onlyvisible` with no name pattern blocks forever, and this
# atari800 quits by itself ~15s after boot unless the monitor keeps taking control.
#
# Env: DISP (default :95)
set -u
DISP="${DISP:-:95}"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$DIR/extracted/palette}"
mkdir -p "$OUT"; rm -f "$OUT"/*.png
FIFO=/tmp/a8pal; rm -f "$FIFO"; mkfifo "$FIFO"
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
           "World Karate Championship.atr" < "$FIFO" > /tmp/pal.log 2>&1 &
PID=$!
exec 3>"$FIFO"
sleep 13
W=$(timeout 5 env DISPLAY=$DISP xdotool search --pid $PID 2>/dev/null | tail -1)
[ -n "$W" ] || { echo "ERROR: emulator window not found"; kill -9 $PID 2>/dev/null; exit 1; }

# Take the machine over.
kill -INT $PID; sleep 0.7
printf 'C 0600 A9 00 8D 0E D4 8D 00 D4 EA EA EA AD 0B D4 D0 FB\n' >&3; sleep 0.20
printf 'C 0610 A2 00 A0 F0 8E 0A D4 8E 1A D0 E8 88 D0 F6 4C 0B 06\n' >&3; sleep 0.20
printf 'C 3901 4C 00 06\n' >&3; sleep 0.20      # VVBLKI -> the ramp, and never back
printf 'CONT\n' >&3; sleep 1.0

# $1 = the colour byte the ramp starts from
pass() {
  kill -INT $PID; sleep 0.35
  printf 'C 0611 %02X\n' "$1" >&3; sleep 0.20
  printf 'CONT\n' >&3; sleep 0.60
  timeout 5 env DISPLAY=$DISP import -window "$W" "$OUT/$(printf 'ramp_%02X' $1).png" 2>/dev/null
  echo "pass from \$$(printf '%02X' $1) -> $OUT/$(printf 'ramp_%02X' $1).png"
}

pass 0x00
pass 0x80

kill -INT $PID; sleep 0.4
printf 'QUIT\n' >&3; sleep 0.4
exec 3>&-; kill -9 $PID 2>/dev/null
echo "DONE -> $OUT"
ls "$OUT"/*.png 2>/dev/null | wc -l
