# World Karate Championship — sprite output and music player internals

Reverse-engineered from `../extracted/ram_full_0000_BFFF.dasm` (the decrypted in-RAM image
of the running game) and verified against live emulator captures in
`../extracted/harvest2/`. Addresses are the game's real run-time addresses.

Everything below is either **verified** (reproduced from a live capture or a dump of the
table itself) or marked **inferred**. Companion documents: `REVERSE_ENGINEERING.md` (overall
architecture), `EXTRACTION.md` (how the disk was opened).

---

# Part 1 — Sprite output

## 1.1 Fighter layout on screen

**Measured from full-frame captures** at exactly 2 host px per Atari colour clock:

- A sprite pixel is **2 colour clocks wide and 1 scanline tall** — the Players run at
  double width. Four adjacent Players give a field of 32 sprite pixels.
- A fighter is **three colours plus transparent**: gi, skin, and a black outline. The
  two fighters differ only in gi colour (white and red), so the poses are stored as
  colour *indices* and the port supplies the gi colour.
- `DMACTL = $3E` → single-line resolution, one byte per scanline per Player.
- Game x maps to screen as **`clock = 2 * x + 28`**, verified against `$E0`/`$E1` in the
  dumps: `$E1 = 140` put the sprite's left edge at clock 308, exactly.

### Two more corrections, from the clipping bug

The first working extraction cut a window out of `$E0` plus the ROM's `$5384` width.
Both halves of that were wrong:

- **`$5384` is not the drawn width.** It is the width `$5509` uses to build a fighter's
  box when clamping it to the arena. Poses draw wider than it: the recovered sprites run
  up to 34 px against a `$5384` of 33, and a high kick's extended leg reaches further
  left than `$E0`.
- **The fighters were parked against the edge of the playfield.** At the old capture
  positions `$24`/`$8C` the right-hand fighter's wider poses were clipped by the screen
  itself, so the data was truncated before extraction even began.

The fix was to place them at `$30`/`$78`, well inside and 72 units apart, and to isolate
the figure as the connected blob of gi and skin containing the red pixels -- nothing
about the width is assumed. Each pose now also carries `x0`, where its ink starts
relative to the fighter's screen origin: the offset varies per pose, and drawing every
pose flush left made the figure jitter as the animation ran.

### The referee

A third figure, on the same Player/Missile hardware but in his own scanline band between
the playfield and the fighters. `$5807` paces him: `$6159` steps by `$615A` (`$58AD`,
normally 2) and turns at `$F0` and `$0A`, and **each turn decrements `$6154`** -- so he
is the round counter as well as scenery.

His graphics are not in the fighters' shape table, and `$595A` only draws the small
pointer arrows that accompany him, so he is captured from the screen instead
(`extract_referee.py`): 9 px by 32 scanlines, agreed by 124 of 185 frames. Frames where
both fighters are on a standing pose leave his band clear, which is what makes him
isolate cleanly.

### Correction to an earlier reading

An earlier pass here concluded that a fighter was "four adjacent Players, 1 bit per
pixel" and that the poses had been extracted correctly from P/M memory. **Both claims
were wrong.** The planes are not 1bpp silhouettes, and the extraction was mislabelled:
parking a fighter on unused shape 4 does not blank it — the P/M buffer keeps its last
pose — so "the occupied band" belonged to either fighter, unpredictably.
`check_shape_ids.py` measured this by matching the fighter actually visible on screen
against every extracted pose: **only 4 of 71 captures matched their label.**

The fix was to drive BOTH fighters to the shape under test (`harvest_v2.sh`) and read
the composited screen rather than the planes, which also recovers the colours. Widths
then agree exactly with the ROM's own `$5384` table for every shape, which is the check
that the labelling is now right.

## 1.2 The P/M area is double-buffered

`$6119` alternates between `$08` (players live at `$0C00-$0FFF`) and `$18` (that buffer
holds stale content), and `$6120` is the base actually programmed into `PMBASE`, forced
to `$00` on menu and scenery screens. This matters when reading P/M memory. It does
*not* matter for screen extraction, which sees whatever GTIA composited.

## 1.3 Shape metadata and data

| What | Where | Note |
|------|-------|------|
| Shape count | **54** (0..53) | id 4 has width/height 0 and is never drawn — used as a "park" value; the store at `$5423` zeroes anything ≥ `$36` |
| frame → shape id | `$55B1` | `FRAME_SHAPE[]`, **199 entries** |
| per-shape height | `$6BC0` | height of the segment-encoded source data, not the on-screen height |
| per-shape width | `$5384` | width in sprite pixels — matches the captured sprites exactly |
| segment struct ptr | `$6800 + id*2` → `$6900 + id*12` | 12-byte struct per shape (pointer table steps by 12 — verified) |
| pixel data ptr | `$6880 + id*2` | all shapes packed consecutively from `$6C00`, `height*8` bytes each, 10,816 B total |

## 1.4 The decode pipeline

The routines are reached through a **thunk table at `$2714–$2786`**, never called directly.
Per frame, the body at `$2E69` runs:

```
jsr $2774 -> $5ABC          ; prologue
jsr $3CAF                   ; build per-fighter pointers from $DD/$DE (shape ids)
jsr $271A -> $45CC

$D2 = $6114 EOR 1           ; the other fighter
  jsr $275C -> $4B13        ; expand segment struct
  jsr $2738 -> $4D10        ; blit
  jsr $2756 -> $4F9C        ; position

$D2 = $6114                 ; this fighter
  jsr $275C -> $4B13
  jsr $2738 -> $4D10
  jsr $2759 -> $4E9F
  jsr $2756 -> $4F9C

jsr $2753 -> $5084          ; ONCE, after both fighters
jsr $31B4
```

Order and the value of `$D2` both matter. In particular `$5084` runs **once at the end**,
not once per fighter. `decode_shapes.py` drives a different order and resets `$D2` before
every routine, so its output is not what the game executes.

`$3CAF` per fighter (`$D2` = 1 then 0):

```
$6A,x/$6B,x = $6800[shape*2]      ; segment struct pointer
$72,x/$73,x = $6880[shape*2]      ; pixel data pointer
$6E,x       = $E0[fighter] >> 2   ; horizontal position / 4  ($E0,$E1 = fighter X)
$6F,x       = $6119               ; destination page = the live P/M half
```

## 1.5 Why a standalone static render is hard

`$4B13` does **not** emit pixels. It walks the bitmap and builds a **sparse segment list**
in two parallel arrays `$0C27[i]` and `$0D27[i]`, counted by `$6113`:

```
STA $0D27,x / STA $0C27,x / TXA / STA ($64),y / INX
```

Those arrays physically overlap the start of the P0/P1 pages, because `$0C27/$0D27` fall
inside the P/M region when PMBASE is `$0800`. So a raw read of P0 during decode shows
sequential values (`$12,$13,$14,$17,$18…`) — that is the segment list, not image data. The
earlier documentation was right about the symptom; the values are positions, not indices into
a pattern dictionary.

Each entry is a **packed address**. `$5084` shifts it left three times, using the carries to
select one of the four Players (`X = $6118 + 0..3`, the page) and the remainder as the low
byte, then **patches those bytes into its own instruction stream**:

```
$50CC  LDX $xxxx,Y      ; operand patched at $50CD/$50CE
       LDA $1E00,X      ; mask table
       AND $yyyy,Y      ; \
       ORA $yyyy,Y      ;  >  operands patched at $50D5/$50D6, $50D8/$50D9, $50DB/$50DC
       STA $yyyy,Y      ; /
       DEY / BPL        ; 8 rows per segment
```

Supporting tables are generated at run time by `$50F6`, not stored: `$6600` is a bit-reversal
table, from which `$1B00` (2-bit-pair horizontal flip, for the mirrored facing) is built.
`$1E00` is the mask table (`FF FC FC FC F3 F0 F0 F0 …` — 2-bit groups).

Net: a pixel-perfect native render means reimplementing the whole pipeline including a
self-modifying blitter. The running game already does all of this, so the shape library is
**captured from it at build time** instead. That keeps the port self-sufficient at run time —
the captured sprites become static data; only the extraction step needs an emulator.

## 1.6 Working extraction procedure

`harvest_v2.sh` + `extract_sprites.py`.

- Emulator flags: `-horiz-area full -vert-area full -image-aspect none -win-width 768
  -win-height 480` → the whole 384×240 frame at exactly 2 host px per Atari pixel, nothing
  clipped. (The default window shows a *crop*, which is why an earlier attempt lost the
  fighters' feet.)
- NOP-patch the frame→shape stores `STA $DD,y` at `$5423` and `$54FC` so a poked shape id
  is frozen, and patch `$5509` to `RTS` so the fighters stop drifting between the poke and
  the screenshot.
- Poke **both** `$DD` and `$DE` to the shape under test, and push the fighters apart via
  `$E0`/`$E1`. Both on-screen figures are then the shape under test, which is what makes
  the labelling unambiguous.
- Capture a screenshot **and** a RAM dump (`$0000-$7FFF`) in the same frozen state; the
  dump supplies `$E1` and the ROM's `$5384` width, giving the sprite's exact x window.
- `extract_sprites.py` isolates the red-gi fighter (the only object on screen in that
  colour — the other fighter and the referee are both white), grows the window over
  coloured rows only, trims solid-outline edges, and mirrors the pose to face right.
- `KEEP=1 TAG=<letter> ATTEMPTS=<n> SHAPES="<ids>"` tops up specific shapes into an
  existing harvest; several runs can be merged.
- `DISP` selects the X display (default `:95`, started automatically). Do not reuse a
  shared `:99` — a wedged X server there produced a long run of confusing failures.
  Note also that `xdotool search --pid N --onlyvisible` with no name pattern **blocks
  forever**; the script uses `--pid` alone, under a `timeout`.
- **Environment quirk:** this `atari800` 5.2.0 build quits by itself about 15 seconds
  after the disk boots if left free-running. Emulation is suspended while the monitor has
  control, so the harvester breaks in early and afterwards runs the machine in short
  bursts.

Status: **all 53 real poses** extracted in colour (`screenshots/shape_atlas_colour.png`),
emitted to `generated/shapes_pm.h`. `check_shape_ids.py` remains as the audit that caught
the first, mislabelled attempt.

---

# Part 2 — Music player

A conventional three-voice tracker: *sequence (order list) → pattern → note + command*, with
per-voice envelope, vibrato and arpeggio, writing through a 9-byte shadow to POKEY. All
addresses and tables below are **verified** against the RAM dump.

## 2.1 Entry points and tempo

| Address | Role |
|---------|------|
| `$1F00` | thunk (`$275F`) |
| `$1F03` | thunk (`$2762`) → `$26CF` |
| `$1F06` | thunk (`$276B`) — **tick entry**, called per frame |
| `$1F11` | player body |

`$1F06` is the tempo prescaler:

```
DEC $2166 / BPL $1F11 / LDA #$0A / STA $2166 / RTS
```

`$2166` is reloaded with 10, so the player runs for divider values 9..0 and skips the
frame on which it underflows: **10 ticks in every 11 video frames**, about 54.5 Hz. That
is the tempo, and it is not adjustable. (An earlier note here said "once every 11
frames", which is off by an order of magnitude.)

`$1F11` tests the state byte `$2167`: bit 7 set → the silence path at `$1F28`, which zeroes
`$D200–$D207` (all four AUDF/AUDC pairs) and sets `$2167 = $80`. Otherwise play at `$1F3A`.

## 2.2 Per-voice state (3 voices, X = 2 → 0)

`$1F3A` does `INC $2149` (a global tick used for vibrato phase) and then loops the voices.
`$C4` holds the voice index; `$C5 = (voice+1)*2` indexes the POKEY shadow.

| Base | Meaning |
|------|---------|
| `$21D0,x` / `$21D3,x` | sequence (order list) pointer, lo/hi → `$C0/$C1` |
| `$214A,x` | position in the sequence |
| `$2150,x` | position within the current pattern |
| `$214D,x` | note duration countdown — non-zero means "keep sounding" |
| `$2153,x` | current note |
| `$2156,x` | envelope / instrument id |
| `$2159,x` | AUDC bits OR-ed into the envelope output (distortion + base volume) |
| `$215C,x` | vibrato depth |
| `$2160,x` | position in the arpeggio table |
| `$2163,x` | arpeggio enable |
| `$C6,x` | current base AUDF value for the voice |
| `$C9,x` | envelope position |
| `$CC,x` | vibrato enable |

The three voice streams are at **`$220C`, `$228D`, `$22B7`** (from `$21D0`/`$21D3`).

## 2.3 Sequence and pattern format

Sequence bytes are pattern numbers, with two escapes:

| Byte | Meaning |
|------|---------|
| `$FF` | end of sequence → rewind (`$214A,x = 0`, `$2150,x = 0`) and loop |
| `$FE` | command → `JSR $26CF` |
| other | pattern number |

A pattern number indexes the **pattern pointer table** `$21D6` (lo) / `$21F1` (hi) → `$C2/$C3`.
There are **27 patterns**.

Inside a pattern, the event byte at `(C2),y`:

- **bit 7 set** → a *command*: `AND #$0F` indexes the jump table `$20C2` (lo) / `$20CA` (hi);
  the address is patched into a `JSR` operand at `$1F9F/$1FA0` and called. After it
  returns, `INY` and the event is re-read, so commands chain until a note is found.
  All eight are now decoded:

  | # | Handler | Effect |
  |---|---------|--------|
  | 0 | `$2093` | set the envelope, `$2156,x` (takes one operand byte) |
  | 1 | `$209A` | vibrato on with depth `$215C,x` (one operand byte) |
  | 2 | `$20A5` | vibrato off |
  | 3 | `$20AA` | `$2159,x = $80` — 17-bit poly, i.e. noise |
  | 4 | `$20AE` | `$2159,x = $A0` — pure tone |
  | 5 | `$20B4` | arpeggio on |
  | 6 | `$20B8` | arpeggio off |
  | 7 | `$20BE` | `$2159,x = $C0` — 4-bit poly |
- **bit 7 clear** → a *note*: `AND #$1F` indexes the **duration table** `$20D2` →
  `$214D,x`. The next byte is the note number → `$2153,x`.

Duration table `$20D2` (9 entries): `06 0C 18 30 60 48 90 3C 12` — a clean binary/dotted
set of note lengths.

A pattern is terminated by `$FF`, which resets `$2150,x` and advances `$214A,x` (next entry
in the sequence).

## 2.4 Pitch

Two pitch paths, chosen by voice:

- **Voice 0 uses 16-bit mode.** The note indexes `$2109` as a word table
  (`$2109 + note*2`), and the result is written to the `$2168/$216A` shadow pair — i.e. two
  POKEY channels joined for 16-bit frequency resolution. Values: `0B38 0A8C 0A00 096A 08E8
  086A 07EF 0780 0708 06AE 0646 05E6 0595 0541 04F6 04B0 …`
- **Voices 1 and 2 use 8-bit mode.** The note indexes `$20DB` → `$C6,x` and the shadow.
  Values: `FF F1 E4 D7 CB C0 B5 AA A1 98 8F 87 7F 78 72 6B 65 5F 5A 55 50 4B 47 43 3F 3C 38
  35 32 2F 2C 2A` (32 entries).

Both tables are **chromatic**: successive ratios are ≈ 0.9439 = 2^(−1/12). `$20DB` spans 32
semitones; the `$7F` at index 12 is exactly half the `$FF` at index 0, confirming one octave
per 12 entries.

## 2.5 Envelope, vibrato, arpeggio

**Envelope** (`$1FF2`): `$2156,x` (instrument) indexes `$217A` (lo) / `$217F` (hi) — five
envelopes, at `$2184 $218C $21B0 $21BB $21CD`. The pointer is patched into an `LDA abs,Y`
operand at `$2007` and stepped by `$C9,x`:

- `$FF` → envelope finished;
- **bit 7 set** → release: zero the shadow entry and advance;
- otherwise `ORA $2159,x` → the AUDC shadow (`$2169,y`), so the envelope supplies volume
  while `$2159,x` supplies the distortion/volume-base bits.

**Vibrato** (`$2033`), gated by `$CC,x`: alternates on `$2149 AND #$01`, taking depth from
`$215C,x`; the depth is subtracted from the current note via an `SBC` whose operand is
patched at `$204E`, and the result is looked up in `$20DB` and written to the frequency shadow.

**Arpeggio** (`$2058`), gated by `$2163,x`: steps `$2160,x` through the table at **`$2171`**
= `00 01 01 00 00 FF FE FF 1F`, where `$1F` is the terminator that wraps the position back to
zero. Each entry is added to the voice's base `$C6,x`.

Finally `$2079` does `DEC $214D,x` and moves to the next voice.

## 2.6 Output

```
$2082:  LDA $CF / BNE rts        ; $CF non-zero = muted, hardware untouched
$2087:  LDX #$08
        LDA $2168,x / STA $D200,x / DEX / BPL
```

A **9-byte shadow at `$2168–$2170`** is copied to `$D200–$D208`, i.e.
`AUDF1 AUDC1 AUDF2 AUDC2 AUDF3 AUDC3 AUDF4 AUDC4 AUDCTL`. AUDCTL is part of the shadow, so
the 16-bit channel pairing for voice 0 is set by the music data itself.

## 2.7 Voice-to-channel mapping, and AUDCTL

`$C5 = (voice + 1) * 2` indexes the shadow, so:

| Voice | AUDF | AUDC | POKEY channels |
|-------|------|------|----------------|
| 0 | `$2168`/`$216A` | `$216B` | 1+2 joined, 16-bit, 1.79 MHz clock |
| 1 | `$216C` | `$216D` | 3, 64 kHz |
| 2 | `$216E` | `$216F` | 4, 64 kHz |

`AUDCTL` is `$50`: bit 4 joins channels 2+1 into one 16-bit channel, bit 6 clocks
channel 1 at the CPU rate. That is why voice 0 has its own 16-bit pitch table and why
`$2020` skips the AUDF write for voice 0 — the note already wrote both halves.

## 2.8 Status: ported

`pokey.h` is the player, statement for statement, and `generated/music.h` is its data,
read out of a dump by `extract_music.py`: 3 sequences, 27 patterns (390 notes and 109
commands), 5 envelopes, both pitch tables and the arpeggio table. The tune runs 225
seconds before it loops.

Note that the recording used for the comparison below was taken during the attract demo,
where `$54 = 1` and `$55 = 0` — music on, effects off — which is why it contains the tune
and nothing else.

`make verify-music` checks it, including a direct comparison against the hardware:
atari800's `-pokeyrec` writes exactly the same nine-byte-per-frame register record that
the player builds at `$2168`, so the two can be diffed. Voice 0's set of AUDC bytes --
the envelope and distortion logic end to end -- comes out **identical** to the
emulator's, and every voice-0 pitch the port emits is a value from `$2109`.

What that comparison cannot show is a frame-for-frame match: that would need the port to
begin at the same point in the tune, in the same per-voice state, as the emulator
happened to be in when recording started, and a register dump does not pin that down.

## 2.9 What a native reimplementation needs

Static data to extract: the 3 sequences, the 27 patterns, the pointer tables `$21D6/$21F1`,
the duration table `$20D2`, both pitch tables `$20DB` and `$2109`, the 5 envelopes, and the
arpeggio table `$2171`.

Code to port: the loop above (straightforward — the only self-modifying parts are operand
patches at `$1F9F/$1FA0`, `$2007` and `$204E`, each of which becomes an ordinary indexed read
or a function pointer), plus a POKEY tone generator honouring `AUDCTL` (15 kHz clock select,
16-bit channel pairing) and the `AUDC` distortion/volume nibbles.

All eight pattern commands are decoded (see the table above). This engine is not the
game's only sound source, though: the **sound effects are digitised samples** played from
a POKEY timer interrupt at `$3AE0`, and starting one sets `$CF`, this player's mute flag,
so the music drops out for its duration. See `REVERSE_ENGINEERING.md`. (An earlier
version of this document claimed there were no effects at all; that was wrong.)

The `AUDCTL` values other than `$50` seen in a hardware recording are `$28`, the OS's
serial clock during disk access, and `$00`, which is what an effect leaves behind after
`$3B2D` clears POKEY.
