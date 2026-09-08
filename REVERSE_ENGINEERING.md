# Recovered algorithm of "World Karate Championship" (Atari 8-bit)

This is the algorithm/architecture reconstructed from `extracted/ram_full_0000_BFFF.dasm`
(the decrypted in-RAM image) plus an opcode-aware hardware-access scan of `ram_full_64k.bin`.
The game is the Atari-8-bit edition of Epyx / System-3 **International Karate** — a
point-scoring two-fighter karate match. The C/SDL2 port in this folder reimplements the
game: its fighter state machine is ported from the 6502, and it embeds the original's own
animation tables, fighter poses and background bitmaps. The code is original C; the data
is not. See README.md.

## Evidence → architecture

Hardware registers actually touched by the code (counts from the opcode scan):

| Subsystem | Registers (hits) | What it means |
|-----------|------------------|---------------|
| **Fighters = hardware Players** | `HPOSP0-3` (7/4/9/4), `SIZEP0-3`, `GRAFP`, `COLPM0-3`, `GRACTL=$03`, `PMBASE $D407` | The two karateka (and referee) are P/M-graphics sprites, positioned by `HPOSPn`, colored by `COLPMn`. |
| **Hit detection** | `HITCLR $D01E` written once at init; a single read of `$D004` (P0PF) in the VBI at `$3888` sets the hit flag `$6155` | Contrary to an earlier note here, the code does **not** poll player-vs-player collisions each frame. |
| **Input** | `PORTA $D300` (10, reads at `$3BC7`, `$5205`, `$5277`, `$5EC5`), `CONSOL $D01F` (15), POKEY `KBCODE $D209`/`IRQEN $D20E`/`SKCTL $D20F` | Two joysticks via PIA PORTA (4 bits dir + trigger). START/SELECT/OPTION via CONSOL. Keyboard via POKEY IRQ (menu / options). |
| **Backgrounds = DLI raster** | `WSYNC $D40A` (29), `VCOUNT $D40B` (15), `NMIEN $C0`, `COLPF0-2`/`COLBK` mid-frame, `DLISTL/H $D402/3` | The beach/temple/mountain scenes are drawn as a character/map playfield whose colours are changed per-scanline by Display-List Interrupts (`WSYNC`+`COLxx`), giving the multi-colour sky/sand gradients. |
| **Sound = 4-ch POKEY** | `AUDF1-4`/`AUDC1-4`, `AUDCTL`, timer-1 IRQ via `IRQEN $D20E` | Two sources that take turns: a three-voice tune, and six **digitised samples** played from a POKEY timer interrupt in volume-only mode. |
| **RNG / timing** | `RANDOM $D20A` (13), VBI+DLI via `NMIEN/NMIST $D40E/F` | RNG drives attract mode + computer AI; frame logic is VBI/frame-locked. |

## Main loop (grounded)

Init routine near **`$3BB7`**: configure PIA (`STA $D302/$D303`), prime `PORTA/PORTB`,
enable P/M DMA (`GRACTL=$03`), set display list, enable interrupts (`NMIEN=$C0`).

Main loop head at **`$3BF9`**, with the frame barrier at **`$3C0A`**:

```
L3C0A:  LDA $D40B      ; VCOUNT
        CMP #$44
        BCS L3C0A      ; spin until raster passes scanline $44  -> one frame tick
        ...            ; then: read sticks, run fighter state machines,
                       ; resolve collisions/scoring, update P/M positions
```

So the per-frame algorithm is:

1. **Wait for frame** (`VCOUNT` barrier — the port uses a 60 Hz fixed timestep).
2. **Read input** for both fighters from `PORTA` (+ trigger). Direction nibble + fire.
3. **Advance each fighter's move state machine** — a move is an id plus an index into the
   animation frame tables; see below.
4. **Resolve strikes** during frames flagged live by `FRAME_ATTR & $08`.
5. **Score & referee**: a clean blow freezes play; the referee awards a half or full point;
   two full points (ippon) win the bout; then advance the background/opponent.
6. **Render**: position Players (`HPOSPn`, `COLPMn`), run DLI colour bands, HUD.

State variables were located in a zero-page/`$61xx` block (e.g. `$6114–$6121`, `$D0`, `$D8`
seen in the `$3BF9` loop) holding per-fighter move id, frame, facing and the score counters.

## The fighter state machine (recovered, and now the port's logic)

Routines: `$53BC` (per-frame update), `$530F` (choose a move), `$5509` (movement),
`$51F9` (input dispatch), `$3F92` (fighter-to-fighter geometry). Ported to `fighter.h`.

**A fighter's state is an index into the per-frame tables.** `$6181,y` holds the move id
and `$EE,y` the animation frame index; `$F0,y` is the next index. Each **game tick** the
index advances by one, and when it reaches `MOVE_FRAME_START[move+1]` the move is over
and the queued move `$EC,y` is taken. A game tick is not a video frame: `$3BF9` waits for
`$6121` to pass `$3BF5[$619B]`, and `$619B` is 0 in all 30 fight dumps, so the divider is
5 and the fighters advance once every **6 video frames**, about 10 Hz, while video, music
and the clock stay at 60. The motion is genuinely that chunky on the real machine. Per
frame:

| Table | Address | Role |
|-------|---------|------|
| `MOVE_FRAME_START` | `$558D` | move → first animation frame |
| `FRAME_SHAPE` | `$55B1` | frame → shape id (≥ `$36` is drawn as 0) |
| `FRAME_VELX` | `$5679` | frame → signed horizontal velocity |
| `FRAME_ATTR` | `$5740` | frame → attribute bits |
| `SHAPE_WIDTH` | `$5384` | shape → width, used to clamp to the arena |

`FRAME_ATTR` bits, from `$5423`–`$54DA`: `$01` turn marker (`$6114`), `$02` flip facing
and set the flip marker `$E4`, `$04` recovery — a held pose that unwinds by playing the
animation **backwards** (`$54E5`), `$08` the frame may be **held** while the same move is
still asked for (`$5444`; an earlier version of this section called it "the strike is
live", which is wrong — it is the hold flag `$6185` counts), `$10` input may be taken,
`$20` lock the fighter (`$618D`; this is what keeps a knocked-down fighter down),
`$40` open the opponent's alternate move set (`$618F`).

**Half of `$53BC` is the joystick path only.** `$53D6` and `$543F` each test `$0050,y`
and jump over their whole block when it is zero. So a CPU-controlled fighter never
unwinds a pose backwards, never stalls on an `ATTR_HOLD` frame, and never re-dispatches
on an `ATTR_INPUT` frame: it plays each move it starts straight through, and the only
thing that ends a move is running off the end of its frame range.

This gate was missing from the port, which ran all three paths for both fighters. The
CPU opponent's movement came apart as a result — poses unwound backwards and moves
restarted partway. It is checked now, at both ends: `verify_fighter.c` asserts the
behaviour and `verify_fighter_dumps.py` asserts the same invariant on the machine, where
across 30 RAM dumps taken during a fight all 60 samples of a CPU fighter have `$6187`
(reverse) and `$6185` (hold) clear.

**Table sizes.** `$558D` stays monotonic for 36 entries, so there are **35 moves over 199
animation frames**, referencing **54 shapes**. `game_data.h` previously carried these
typed out by hand and truncated to 21 moves and 120 frames, so most of the animation set
was missing from the port. They are now generated by `extract_tables.py`.

**There is no vertical motion.** `$5509` only ever touches `$E0/$E1`, the horizontal
position. Jumps, somersaults and falls are drawn into the poses themselves — each pose
carries its own absolute top scanline. Any gravity or jump physics in a port is invented.

**Movement and the arena.** `$5509` adds `FRAME_VELX[frame]` to `x`. The sign and the
bounding box come from one flag, tested at `$550E`: clear adds and gives the box
`[x, x + SHAPE_WIDTH]`, set subtracts and gives `[x + $24 - SHAPE_WIDTH, x + $24]`. On
the normal path that flag is the facing (`$550B`); on the reverse path it is
`reverse XOR facing` (`$54FF`), so an unwinding pose slides back the way it came.

The clamp keeps the box inside `$10..$AE`, and where it parks depends on the facing, not
on the flag. The four landings are `$10` and `$AE - width` facing right (`$5549`/`$556F`),
and — note the constants, they are not symmetric — `$12 + width - $24` and `$8A` facing
left (`$5551`/`$557B`). `$5580` then maps any `x >= $F0` to 0, catching the underflow.
All four are checked in `verify_fighter.c`.

**Input dispatch** (`$51F9`): `index = (fire << 4) | stick_nibble`, the Atari stick being
active-low (bit0 up, 1 down, 2 left, 3 right). The table is chosen by
`facing XOR flip-marker` — `$528F` (R) or `$52AF` (L) — and swapped for the alternate
close-quarters pair `$52CF`/`$52EF` when the fighters are close (`$6136 < 8`), the
opponent's input gate is open, and `MOVE_GATE[$6138]` allows it. `$6138` is a 3-bit
situation code built by `$3F92` from who is on the right and both facings. The result
goes to `$EC,y`, the same slot the AI writes.

**Reaction to a blow** (`$2FFA`/`$3004`): play freezes (`$D8`), and the struck fighter is
forced into move **17** if the blow came from the front or **18** if from behind.

### The CPU opponent (`$3D04`), ported in full

Runs once per CPU fighter per game tick, immediately before that fighter's state machine
(`$51C0` sequences the pair, and which of the two goes first alternates with `$6115`).
It decides on a move, writes it to `$EC,y` — the same slot the joystick path uses —
starts it at once via `JSR $530F`, then clears the slot, so the state machine that
follows sees an empty queue. Going in through `$EC` matters: `$530F`'s queued branch also
re-arms the idle counter `$618B` (`$535A`), and a port that forces the move instead
leaves that counter running until `$5337` fires off taunts in the middle of a fight.

**`$3D04` does not interrupt a move.** `$3D1B` returns immediately unless the fighter's
move is 0 or the current frame has `ATTR_INPUT`, so the AI only ever chooses at a point
where the fighter is free to act. Ported to `ai.h`; the tables are generated into
`generated/frames.h`.

Its inputs are the gap `$6136`, the situation code `$6138`, the opponent's current
move, and the skill level `$F6` (0..5, raised as the player advances and wrapped back
to 1 at `$2C99`). The decision tree:

| Step | Test | Outcome |
|------|------|---------|
| `$3D16` | mid-move and not on an `ATTR_INPUT` frame | do nothing |
| `$3D2A` | `AI_MOVE_FLAG[opponent move]` has bit 7 | defer to `$3DCA` |
| `$3D39` | opponent has been holding a pose ≥ 8 frames (`$6185`) | defer |
| `$3D46` | gap ≥ 9 | jump to the idle gate |
| `$3D50` | opponent crouching (move 5) or low-attacking (12), gap < 8, and `RANDOM ≥ AI_RND_CROUCH[skill]` | reply from `AI_ANTI_CROUCH[(orientation << 3) + RANDOM&7]` |
| `$3D96` | `RANDOM < AI_RND_CLOSE[skill]` | idle gate |
| `$3D96` | `AI_SIT_B[$6138] == 0` | near/far branch |
| `$3DAE` | otherwise | reply from `AI_REPLY0`/`AI_REPLY1` by `AI_MOVE_FLAG[opponent move]`, random column of 8 |
| `$3DCA` | not standing | do nothing; idle ≥ `$30` skips the gate |
| `$3DDB` | `RANDOM < AI_RND_IDLE[skill]` | do nothing |
| `$3DE8` | `AI_SIT_A[$6138] != 0` | `AI_FAR` if gap ≥ 8 else `AI_NEAR`, random column of 12 |
| `$3E08` | gap ≥ 10 | `AI_MID`, random column of 12 |
| `$3E1A` | otherwise | `AI_BY_RANGE[AI_ROW[gap] + random column]`, the column count coming from `AI_COLS[skill]` |

`$3F71` is the random-in-range helper: it draws `RANDOM & $1F`, and if that is not below
the limit ANDs it with `$0F` and with `VCOUNT` and tries again, looping until it fits.

`$3EE8` (`AI_BY_RANGE`) is the heart of it — 10 rows of 12 moves indexed by range, so
the CPU's repertoire changes with distance, and how much of each row it can reach is
what the skill level controls.

### `$08` is a HOLD attribute, not "strike is live"

`$5440` reads `FRAME_ATTR & $08` and, while the same move stays queued, **stalls the
animation on that frame** and counts the stall in `$6185` (saturating at `$80`). That is
how crouches and blocks are held. The block runs only when `$0050,y` is nonzero — i.e.
for a joystick-controlled fighter; a CPU fighter never stalls. `$0050,y` is the same flag
that selects joystick reading in `$51F9` and the AI in `$3D04`.

## Move & scoring tables (generated into `generated/frames.h`)

The tables are read straight out of a RAM dump by `extract_tables.py` and used directly:

- **Input→move dispatch** (`$528F`/`$52AF` base, `$52CF`/`$52EF` alt; 32 bytes each).
  The routine at `$51FA` builds `index = (fire<<4) | stick_nibble` (Atari stick is
  active-low: bit0=up,1=down,2=left,3=right) and reads a move id from the facing-appropriate
  table. Recovered mapping (facing right, no fire): U→jump, D→crouch, R→walk-fwd, L→walk-back,
  UR→jump-fwd, UL→jump-back, DR→duck-fwd, DL→roll-back; with fire: U→9, R→11 (roundhouse),
  D→13 (sweep), etc. Move ids **9–16 and 20 are the attacks**; 17–19 are hit/fall reactions.
- **Move→animation frame ranges** (`$558D`): e.g. stand=2f, jump=6f, roundhouse(11)=6f,
  sweep(13)=5f, back-kick(15)=8f. The port derives each move's duration from these.
- **Per-frame tables** (120 frames): attribute/behaviour bits (`$5740` — `$02` flip-facing,
  `$10` input-acceptable, `$20` move-locked, `$40` input-gate), signed horizontal velocity
  (`$5679` — drives the lunges: jump-back +17, sweep +25), and shape id (`$55B1`).
  The port's fighter movement is taken straight from `FRAME_VELX`.
- **Scoring** — the technique that lands is worth a point toward *ippon*; the numeric bonus is
  a **time bonus** computed at `$311E`: `bonus = (DF≥0x28) ? 1 : (10 − DF/4)`, doubled when
  ≥10 (this is the on-screen "FULL POINT 800"). Implemented as `score_time_bonus()` and shown
  as the point BONUS in the port.

`extract_tables.py` records the originating address of each table.

## Sprite/shape system (located in full; format is segment-encoded)

Fully traced from the code:

- **Shape count/metadata:** **54 shapes** (0..53; the store at `$5423` zeroes anything ≥ `$36`).
  `frame → shape id` is `$55B1`; per-shape width is `$5384` and the height of the
  segment-encoded source data is `$6BC0`. Shape 4 has width and height 0 and is unused.
- **Data pointers** (`$3CAF`, indexed by `shape*2`): `$6800[s]` → a 12-byte segment struct;
  `$6880[s]` → the pixel data (all shapes packed consecutively from **`$6C00`**, `height*8`
  bytes each — 10,816 bytes total). Transcribed to `shapes_data.h` / `extracted/shapes_raw.bin`.
- **Colour:** 2-colour fighter (skin `COLPM0=$1E`, red `COLPM1=$34`).
- **Decoder:** `$4B13` reads the segment struct (nibble = per-segment width/height) and expands
  the encoded pixel data into a work buffer; `$4D10` blits it row-by-row into the P/M area,
  with a horizontal-flip lookup at `$1B00` for the mirrored facing.

### Fighter layout in P/M (verified against live captures)

- One fighter spans **all four Players placed adjacently — 32 px wide, 1 bit per pixel**
  (not a 2bpp player pair). Rendering a live capture's `P0|P1|P2|P3` side by side as 1bpp
  yields an unmistakable karateka; every other interpretation yields noise.
- **Both** fighters share those same four Players and are separated **in time**, into two
  scanline bands (~39–70 and ~190–230), by per-DLI `HPOS` reloads. A capture with one
  fighter parked on unused shape 4 therefore contains exactly one occupied band.
- **The P/M area is double-buffered.** `$6119` alternates between `$08` (players live at
  `$0C00–$0FFF`; the frame is usable) and `$18` (that buffer holds stale content). Any
  extraction must check `$6119` and discard the wrong parity — reading a fixed `$0C00`
  unconditionally is what made earlier attempts look "distorted/non-human".

### Real call sequence (`$2E69`)

The routines are reached through the thunk table at `$2714–$2786`, not called directly.
Per frame the game runs, via `$2774`→`$5ABC`, `$3CAF`, `$271A`→`$45CC`, then:

```
$D2 = $6114 EOR 1        ; the other fighter
  $275C->$4B13   $2738->$4D10   $2756->$4F9C
$D2 = $6114              ; this fighter
  $275C->$4B13   $2738->$4D10   $2759->$4E9F   $2756->$4F9C
$2753->$5084             ; ONCE, after both fighters
```

Order and `$D2` state matter: `$5084` runs once at the end, not per fighter.
`decode_shapes.py` drives a different order (and resets `$D2` before every routine), so
its output is **not** the sequence the game executes.

The pixel data at `$6C00` is **not** a flat bitmap — it is segment-encoded, so a naïve
row-major render is noise. Rather than reverse the format by hand, `cpu6502.py` (a small
6502 core) + `decode_shapes.py` **execute the game's own decode routines** over a real RAM
snapshot, per shape id:

    $3CAF  build $6A/$72 source & bitmap pointers from $DD (shape id)
    $4B13  expand the $6800 segment struct -> $82/$8E lists, set $E6
    $4D10  blit the bitmap into the $1008 work buffer (uses the $1B00 h-flip table)
    $4F9C / $4E9F / $5084  position the segments into the P/M player buffers

All 47 real shapes (shape 4 is unused) decode distinctly this way — see
`screenshots/shape_decode_atlas.png`.

**Boundary (why a standalone static render is impractical).** `$4B13` does not emit pixels.
It walks the bitmap and builds a **sparse segment list** in two parallel arrays,
`$0C27[i]` and `$0D27[i]`, counted by `$6113` (`STA $0D27,x / STA $0C27,x / TXA /
STA ($64),y / INX`). Each entry is a *packed address*, not image data: `$5084` shifts it
left three times, using the carries to pick one of the four Players (`X = $6118 + 0..3`,
the page) and the remainder as the low byte, then **patches those bytes into its own
instruction stream** at `$50CC–$50DC`:

```
$50CC  LDX $xxxx,Y     ; operand patched from the segment entry
       LDA $1E00,X     ; mask table
       AND $yyyy,Y     ; \
       ORA $yyyy,Y     ;  > operands patched likewise
       STA $yyyy,Y     ; /
       DEY / BPL       ; 8 rows per segment
```

`$1B00` (2-bit-pair horizontal flip) and the `$6600` bit-reversal table it is built from
are generated at run time by `$50F6`.

So the observation that the buffers hold "sequential values, not pixels" is correct — the
sequence `$12,$13,$14,$17,$18…` is the segment list, which physically overlaps the start of
the P0/P1 pages because `$0C27/$0D27` sit inside the P/M region (PMBASE `$0800`). The
practical conclusion also stands: a pixel-perfect static render means reimplementing the
whole pipeline, self-modifying blitter included. The running game already does all of this,
so the shape library is captured from it instead (below).

### Deterministic capture of the whole shape library (`harvest_shapes.sh`)

To get every shape's pixel-perfect graphics without reimplementing the renderer, the harvester
lets the **real GTIA** render each shape on demand. In the running game it NOP-patches the
frame→shape stores (`STA $DD,y` at `$54FC` and `$5423`) via the monitor, which *freezes* each
fighter's shape id; then it pokes `$DD/$DE` to every id 0..47 in turn, lets the game render one
frame, and dumps P/M. Each fighter's silhouette is read from the P/M players (bottom scanline
band, largest connected component) and labelled by the poked id.

**Important correction — read the SCREEN, not the P/M buffers.** An early atlas was built by
reading the raw P/M *player buffers* and tiling them; the results looked distorted/non-human
because the game **multiplexes the players per-scanline via DLIs** (their `HPOS`/`SIZE` change
mid-frame), so a static buffer read cannot reconstruct the real sprite. The correct source is
the **composited screen** — what GTIA actually outputs.

`screen_harvest.sh` freezes the frame→shape store (as above), forces each shape, and
**screenshots the emulator window**; a background frame (fighter blanked) is subtracted to
isolate the fighter. This yields genuinely human karateka in every pose — see
`screenshots/full_sprite_atlas.png`.

### Current extraction path (`harvest_v2.sh` + `pick_shapes.py`)

The screen route has two defects: the default window shows a **crop** of the frame (the
fighter's feet fall outside it), and the scenery is animated, so a single background frame
does not subtract cleanly. `harvest_v2.sh` fixes the geometry and takes the sprite from
P/M instead of the screen:

- `-horiz-area full -vert-area full -image-aspect none -win-width 768 -win-height 480`
  — the whole 384×240 frame at exactly 2 host px per Atari pixel, nothing clipped.
- Per shape it captures **both** a screenshot and a RAM dump (`$0000–$7FFF`) in the same
  frozen state, so screen pixels and machine state can be correlated.
- Because of the `$6119` double-buffer parity above, roughly half of all captures are
  unusable. Each shape is harvested several times (`ATTEMPTS`), and `pick_shapes.py`
  selects a capture with `$6119 == $08`, subtracts the parked-fighter baseline, isolates
  the lower scanline band, and emits `screenshots/shape_atlas_v2.png`.
- `KEEP=1 TAG=b SHAPES="…"` tops up specific shapes into an existing harvest.

Only the **lower** scanline band is usable: the segment list at `$0C27/$0D27` lies inside
the P0/P1 pages at row 39, exactly where the upper band starts, so upper-band captures read
as dithered stripes. `pick_shapes.py` scores rows ≥ 100 only.

Status: **all 47 shapes** extracted as clean 32×~40 1bpp poses, emitted to
`generated/shapes_pm.h`. Full details in `SPRITE_AND_MUSIC_INTERNALS.md`.

Feeding those screen sprites back into the C port is imperfect to automate (the two fighters
merge, the HUD overlaps, and the frozen fighter drifts low so standing poses clip at the feet),
so the port renders the **clean poses captured from natural gameplay** (`sprites.h`, via
`MOVE2SPR`, animated by move) with the procedural articulated figure as fallback — both look
correct/human. `shapes_raw.bin` + `cpu6502.py`/`decode_shapes.py` remain as the exact source
data and the CPU-level (partial) decoder.

## Music and bout timing (both ported)

**Correction.** An earlier pass here concluded the game had *no* sound effects. That was
wrong, and wrong for two reasons worth recording: the scan for writes to the player's
state covered only `$2149-$2170`, and the interrupt handler at `$3AE0` was dismissed as
"housekeeping" without being read. The empirical check was misleading too — it was taken
during the attract demo, which is precisely the mode that runs with effects disabled.

**Sound effects are digitised samples.** `$3A7C` starts one:

```
CPX #$06 / BCS out        ; six effects
LDA $55  / BEQ out        ; effects disabled
LDA $619D/ BNE out
LDA $3A64,x -> patch the end-page compare at $3AFC
LDA $3A76,x                ; bit 7 selects which nibble, by patching $3B1C-$3B1F:
                           ;   set   -> four LSR A   (high nibble)
                           ;   clear -> AND #$0F     (low nibble)
LDA #$10 / STA $D200       ; AUDF1: the timer rate
JSR $3B2D                  ; zero $D201-$D209, so AUDCTL becomes 0 (64 kHz, unjoined)
LDA $3A6A,x*2 -> patch the sample cursor at $3B1A/$3B1B
LDA #$01 / STA $D20E       ; enable the POKEY timer-1 interrupt
JSR $2765 -> $27E3         ; $CF = $FF: MUTE THE MUSIC
```

The handler at `$3AE0` then fires at `63921 / (AUDF1 + 1)` = about **3760 Hz**, advances
the cursor, takes its nibble and writes it to `AUDC1`, `AUDC2` and `AUDC3` with bit 4
set — POKEY's volume-only mode — so the nibble is the speaker level on three channels at
once. When the cursor's high byte reaches the end page it disables the interrupt and
calls `$2768` → `$27E8`, which clears `$CF` and lets the music back in.

That mute flag is why an effect **interrupts** the music instead of mixing with it.

Six effects over three byte ranges, `$9F00-$AFFF`, each range read once for its high
nibbles and once for its low ones:

| Effect | Bytes | Nibble | Length |
|--------|-------|--------|--------|
| 0 | `$9F00-$A5FF` | high | 0.48 s |
| 1 | `$A600-$ABFF` | high | 0.41 s |
| 2 | `$AC00-$ADFF` | high | 0.14 s |
| 3 | `$9F00-$A4FF` | low  | 0.41 s |
| 4 | `$A500-$AAFF` | low  | 0.41 s |
| 5 | `$AB00-$AFFF` | low  | 0.34 s |

**What fires them** (`$399D`, every frame): if a fighter's shape id changed, look the new
shape up in `$39D3` — 11 ids: 10, 7, 6, 18, 20, 21, 22, 32, 47, 43, 39 — and pick the
effect by game state. While fighting and not frozen, `$39DF`, but only if the frame's
attribute bits pass `AND #$4D`. While the referee has play frozen, `$39EA`, which may
also arm a second effect after a delay (`$39F5` frames, effect `$3A00`, counted down in
the vertical blank at `$395F`). In states 2 and 5, `$3A0B`. `$FF` means silence.

**Music and effects are separately switchable, and the demo differs from play.**
`$0054` enables the music and `$0055` the effects. The vertical blank skips the music
tick entirely when `$54` is zero. Starting a game (`$3312`) reads `CONSOL`: START gives
one player, SELECT two, and either way `$3337` does

```
STX $50 / STY $51          ; who is human
STX $55                    ; effects ON
LDA #$00 / STA $54         ; music OFF
JSR $2762                  ; and stop it
```

so the attract demo runs with music and no effects, and play runs with effects and no
music. Both are then togglable from the keyboard (`$337D`): KBCODE `$2D`/`$AD` turn the
music on/off, `$3E`/`$BE` the effects — unshifted enables, shifted disables. Confirmed on
hardware: in the demo `$54 = 1, $55 = 0`; after pressing START, `$50/$51 = 1/0` and
`$54 = 0, $55 = 1`.

Ported to `sfx.h`, with the samples and tables generated into `generated/sfx.h` by
`extract_sfx.py`; `make verify-sfx` exercises it and renders each effect to a WAV.

**The tune.** One three-voice piece drives the whole game; `$21D0`/`$21D3` are never
rewritten. `$26C0` starts it (`$2167 = $40`), `$26CF` stops it (`$2167 = $C0`), and
`$1F06` ticks it from the vertical blank. The prescaler `$2166` reloads with 10 and skips
the frame it underflows on, so the player runs **10 ticks in every 11 frames**, about
54.5 Hz. The piece is 225 seconds long before it loops. Full detail in
`SPRITE_AND_MUSIC_INTERNALS.md`; ported to `pokey.h` with its data in
`generated/music.h`.

**The game does not run at the video frame rate.** The main loop's barrier at `$3BF9` is
two waits, not one:

```
LDY $619B / LDX $3BF5,y      ; the frame divider for the current speed setting
LDA $D8 / BEQ +  / LDX #$06  ; a longer one while the referee has play frozen
CPX $6121 / BCS -            ; spin until the vertical blank has counted past it
LDA $D40B / CMP #$44 / BCS - ; then sync to the raster
STA $6121 -> 0
```

`$6121` is incremented once per video frame by the vertical blank, so a game tick only
happens once `$6121` exceeds the divider. `$3BF5` holds `5, 4, 6, 7` and `$619B` selects
one, defaulting to the first — so **the fighters advance once every 6 video frames, about
10 Hz**, and once every 7 while play is frozen. A hidden key sequence (`$33C3`) picks a
different entry.

Video, music, the round clock, the referee and the sound watcher all still run at 60 Hz
from the vertical blank; only the fighter logic is divided. Getting this wrong makes a
port run six times too fast, which is exactly what happened here before it was found.

**Bout timing.** `$00DF` is a frame counter bumped by the vertical blank, and the bout
code at `$2F5F..$3029` simply spins on it, so its comparisons are the durations:

| Constant | Frames | Where |
|----------|--------|-------|
| ready hold, before the fighters appear | `$50` = 80 | `$2F9A` |
| hold until the bout is enabled (`$615D`) | `$C8` = 200 | `$2FA9` |
| freeze after a point, and after time runs out | `$80` = 128 | `$2FEA`, `$3017` |
| both fighters' starting x | `$54` | `$2F7C` |

**The round clock is the referee.** `$6154` does not count seconds — it counts referee
traversals. The referee's x (`$6159`) starts at `$28` or `$DC` and steps by `$615A`
(`$58AD`, normally 2) every frame until it passes `$F0` or drops below `$0A`, and each
turn decrements `$6154` (`$5834`). `$2F3A`, indexed by the round number `$615F`, gives
the traversals per round: **8, 15, 20** — so roughly 13, 25 and 33 seconds. Generated
into `generated/timing.h`.

## The HUD (recovered pixel-exactly)

Two rows of **ANTIC mode 4** at scanline 13, from `$61C0`, with the character set at
`$6400` (`CHBASE = $64`). Mode 4 gives four colours per character — two bits per pixel,
four pixels wide — the value selecting `COLBK`, `COLPF0`, `COLPF1`, and then `COLPF2` or
`COLPF3` according to bit 7 of the character code. The charset carries several copies of
the same glyphs drawn in different pixel values, which is how one playfield yields text
in several colours: `$00-$09` digits in value 3, `$0B-$24` letters, `$2C-$35` taller
digits in value 2, `$28-$2B` D/E/M/O in value 1, `$36-$38` L/T/I in value 1.

Colours were recovered by inverting a capture, as the playfield's were, and reproduce it
with **0 differing pixels** (`make verify-hud`). The ippon markers over columns 7-8 and
31-32 are Player/Missile objects built by `$31E1` from `$324C`, not playfield, and are
excluded from that comparison.

**Layout**, from the HUD routine at `$5BB2..$5C8B`:

| Where | What | Source |
|-------|------|--------|
| col 10 | `1UP`, blank when player 1 is not human | `$5BA4`/`$5BA6`/`$5BA8` |
| col 27 | `2UP`, the same with bit 7 set (COLPF3) | |
| col 14 | `TIME` while anyone plays, else `DEMO` | `$5BAA` / `$5BAE` |
| col 19 | two big digits: the clock, BCD, leading zero blanked | `$5C71` |
| col 23 | one player: `L` and the level in COLPF3 digits | `$5C2A` |
| | two players: three markers filled from each end by wins | `$5BFF` |
| row 1, col 14 | the belt name, 12 characters | `$5FB8` |
| cols 3-5 / 37-39 | the scores, right-aligned | |

Belt names live at `$5EFF`, offsets in `$5F53`, stored with `$36` added to each code:
**WHITE, YELLOW, GREEN, PURPLE, BROWN, BLACK**.

**The clock** is `$00DC`: a BCD value decremented by one every `$3C` = 60 frames at
`$3988`, started at `$30` (30 seconds) for one player or `$60` (60) for two
(`$2D25`/`$2D3D`). Note this is a *different* counter from `$6154`, the referee
traversals that actually end the bout.

**Starting a game** (`$3312`) reads `CONSOL`: **START** gives one player and **SELECT**
two. Either way `$3337` sets `$50`/`$51`, turns the effects on and the music off.

## Backgrounds (recovered pixel-exactly)

All seven stages — Sydney, New York, Rio de Janeiro, Japan, Egypt, Greece, Mount Fuji —
are in the port as the game's own data. See `generated/scenes.h`.

### Where the pixels live

The display list at `$6260` (`JVB` back to itself) lays out: 13 blank scanlines, two
ANTIC **mode 4** HUD rows (LMS `$61C0`), 2 blank, then **97 rows of ANTIC mode E**
(LMS `$B000`) — 160 pixels per row, 2 bits per pixel, 40 bytes per row — then more mode-4
rows for the bottom status area, with 11 DLIs scattered through it. So the scene is a
plain bitmap: `97 x 40 = 3880` bytes at `$B000`. Confirmed: exactly 3816 of those 3880
bytes are non-zero and almost nothing follows them.

The scene bitmaps are also **directly readable from the disk image**, no emulator needed:

- `$613D` is the level index, masked `AND #$07`.
- `$449D` is the per-level start sector: `$130 $150 $170 $190 $1B0 $1D0 $1F0`
  (304, 336, 368, 400, 432, 464, 496) — seven levels; the eighth entry repeats the first.
- `$438C` is the game's own "advance to the next level": it bumps `$5C` modulo 7 into
  `$613D`, reads 4096 bytes into `$0400-$13FF`, then copies them to `$B000`.

Verified: the first 3880 bytes of the 4096-byte block at sector 304 are byte-identical to
the `$B000` content captured while the Sydney stage was on screen.

### Where the colours live, and how they were recovered

The colour kernel is a **chained, self-modifying DLI**: each handler installs the next by
writing `VDSLST` (`$0200/$0201`), and patches its successor's operands from tables — `$35AB`
supplies display-list addresses to modify, `$35C0`/`$35D5` the low/high bytes of the routine
that sets that band's colours. The colour values themselves are immediates scattered through
those `$36xx`/`$37xx` routines.

Rather than reverse that, the colours were recovered by **inverting the hardware's output**
(`invert_palette.py`). For each scanline the pixel *values* are known from `$B000` and the
pixel *colours* are known from a full-frame capture, so the active registers can be read off
directly. Two findings fell out of it:

- The alignment solves to **x0 = 32 colour clocks, y0 = 31 scanlines** — exactly the
  standard playfield origin and the display list's own row count. Independent confirmation
  that the model is right.
- Values 1..3 are constant per scanline, but **value 0 is not**: the game rewrites `COLBK`
  *mid-scanline* to paint horizontal bands, averaging about two changes per line. Modelling
  value 0 as x-runs took the reconstruction from 2.1% wrong to exact.

### Verification

`make verify-scenes` renders every stage through the port's own `sceneDraw()` and compares
it against the model recomputed from the captures. The chain

    emulator capture  ==  recovered model  ==  the port's renderer

holds for all seven stages with **0 differing pixels**.

Capture is reproducible with `capture_scenes.sh`, which drives level loading through the
monitor's `R 438C` rather than playing through the game.

## What is faithful vs. approximated

**Recovered from the original, and verified:**

- The **backgrounds** — all seven stages, pixel-exact (`make verify-scenes`, 0 differing
  pixels against the emulator capture).
- The **fighter poses** — all 47 shapes, the game's own Player/Missile bitmaps
  (`generated/shapes_pm.h`).
- The **data tables**: input→move dispatch, per-move animation frame ranges, the
  199-entry per-frame attribute / velocity / shape tables, the CPU opponent's decision
  tables, the music, and the bout timings — all generated from a dump, none typed by hand.
- The **CPU opponent** (`$3D04`), ported branch for branch.
- The **music** (`$1F06`) and its tempo.
- The overall structure: frame-locked loop, collision-based height-gated hit detection,
  half/full-point (ippon) scoring, referee freeze, round timer, stage progression.

**Still approximated:**

- **Hit detection.** The original sets its hit flag from a GTIA collision register read
  in the vertical blank (`$3888`); the port uses sprite overlap instead.
- **The HUD and the title/options/high-score screens**, which are the original's ANTIC mode 4
  text rows and have not been extracted.
