# World Karate Championship — Linux / SDL2 port

A C/SDL2 port of the Atari 8-bit game whose **logic is the original's**, ported from the
6502, and whose graphics are the original's own data. See `REVERSE_ENGINEERING.md` for
how the game works and what maps to what.

## The game itself is not in this repository

The disk image, the dumps taken off it and the emulator snapshot are verbatim copies of a
commercial program — *World Karate Championship*, Datasoft, 1984 — and are not here.
Bring your own if you want to re-derive anything:

| put here | what it is | made by |
|---|---|---|
| `World Karate Championship.atr` (repo root) | the disk image | — |
| `extracted/ram_true_64k.bin` | a 64K RAM dump of the running game | the emulator's monitor |
| `extracted/scenes/*.bin` | per-scene RAM dumps | `port/capture_scenes.sh` |
| `extracted/colour4/`, `extracted/signs/`, … | screen captures | `port/harvest_v2.sh`, `port/harvest_signs.sh` |

**The port builds and runs without any of it** — everything under `port/generated/` is
committed. What needs the originals is re-running the extractors and the parts of
`make verify` that compare against the real machine; those skip with a message when the
data is absent rather than failing.

Note that `port/generated/` still holds the game's own sprites, scenes, music and tables.
This is not a clean-room reimplementation; see the note at the end of this file before
redistributing anything.

The fighter state machine is the game's: a fighter's state is an index into its 120-plus
animation frames, and `FRAME_ATTR` / `FRAME_VELX` / `FRAME_SHAPE` drive everything. There
is no invented move table, no tuned durations, and no jump physics — the original has
none; jumps are drawn into the poses. Nor does the game run at the video frame rate: its
main loop takes a tick only every sixth frame ($3BF9), so the fighters move at about
10 Hz while everything else stays at 60. The CPU opponent is the original's too: `$3D04`
ported branch for branch, driven by its own decision tables and skill levels. So is the
background music, and the bout's timings: the ready and freeze holds are the ROM's frame
counts, and a bout runs until the clock `$00DC` reaches zero or a fighter has four
points — which is what `$2BA2` tests, and the only thing that ends one.

Sound is the original's too, and there are two sources that take turns. The music is a
three-voice POKEY tune; the effects are six **digitised samples** played from a timer
interrupt in volume-only mode, and starting one mutes the music for its duration, exactly
as the hardware does. The attract demo runs with music and no effects; starting a game
turns effects on and the music off; `M` and `N` toggle them, as two keys do on the
original.

**This is no longer a clean-room reimplementation.** The port now embeds data recovered from
the original game: its animation and dispatch tables in `generated/frames.h`, all 53 of its
fighter poses in colour in `generated/shapes_pm.h`, and all seven of its background stages
in `generated/scenes.h`. The *code* is original C; the *data* is the 1998 game's. Bear
that in mind before redistributing.

All seven of the original's stages are present as its own bitmap data, rendered the way
ANTIC drew them, and so is the HUD — the same character set, the same fields in the same
columns, the same colours. `make verify-scenes` and `make verify-hud` check both against
emulator captures with zero differing pixels.

## Build & run

```sh
sudo dnf install SDL2-devel        # Fedora   (Debian/Ubuntu: libsdl2-dev)
make
./worldkarate                      # or: make run
```

## Controls

| | Direction | Fire (attack modifier) |
|---|---|---|
| **Player 1** | `W A S D` | `Left Shift` |
| **Player 2** | Arrow keys | `Right Shift` |

`F1` START — one player against the computer · `F2` SELECT — two players
`M` music · `N` sound effects · `P` pause · `F10` quit

`F1` and `F2` stand in for the console keys the original reads from `CONSOL`.

It opens on the high-score table with these bindings above it, holds that for fifteen
seconds, and then plays a bout against itself with the music on and the effects off,
until you press one of them. Lose a match and the referee holds up MATCH OVER; if your
score makes the table you put three characters in with the movement keys -- left and
right pick a letter, fire takes it -- and then the demo comes back round.

The table is kept in `worldkarate.scores` beside the binary, which is the port's own
doing: the original's lives in RAM and goes with the power. It is plain text, so it can
be read or thrown away by hand; anything the port cannot make sense of in it falls back
to the empty table the game starts with.

Run `make verify` to check both halves: the backgrounds against emulator captures, and
the fighter state machine against its own invariants.

Direction **+ Fire** selects the strike; direction **alone** moves/defends:

- fire + forward → roundhouse (high) · fire + up → face punch (high) · fire + down → foot sweep (low)
- fire + back → back kick · fire + neutral → front kick · forward-jump + fire → flying kick
- no fire: walk, jump (up/fwd/back somersault), crouch, roll

## Rules (as recovered)

- A clean, correctly-aimed blow scores **WAZA-ARI** (half) or **IPPON** (full). Two full
  points win the bout. Defence is positional — **duck** under high attacks, **jump** over
  low sweeps.
- 30-second round timer; if it expires, most points wins.
- Winning advances the **DAN** rank and the stage, through the original's seven locations
  (Sydney → New York → Rio de Janeiro → Japan → Egypt → Greece → Mount Fuji). Clear them
  all for the **black belt**.

## Authentic sprites

All **53 fighter poses** are the game's own, captured in colour from the composited screen
of a running emulator (`harvest_v2.sh` + `extract_sprites.py`) and animated through the
ROM's own `FRAME_SHAPE[]`. A sprite pixel is two colour clocks wide and one scanline tall;
pixels are stored as indices (gi / skin / outline) so each fighter gets its own gi colour.
Every pose also carries its absolute top scanline, which is the whole of a sprite's
vertical placement.

The referee is there too, captured the same way. He signals from the spot: `$6159` is
how far through one of his three signalling actions he is, not where he is, and finishing
one is what takes a tick off the round counter — so a round is a number of his actions.

`check_shape_ids.py` exists because a first attempt at the poses — reading the
Player/Missile planes instead of the screen — came out **mislabelled**, and that script
is what caught it. A later attempt clipped every pose by deriving its width from `$5384`,
which turns out to be the arena-clamp width rather than the drawn one. See
`SPRITE_AND_MUSIC_INTERNALS.md`.

## Files

- `worldkarate.c` — the port
- `fighter.h` — the original's fighter state machine, ported from the 6502
- `ai.h` — the original's CPU opponent (`$3D04`), ported branch for branch
- `pokey.h` — the original's music player (`$1F06`) and enough of POKEY to hear it
- `generated/music.h` — the tune: 3 sequences, 27 patterns, 5 envelopes, both pitch tables
- `extract_sfx.py` — regenerates the sound-effect data
- `generated/timing.h` — the bout's frame counts and the referee-driven round clock
- `verify_music.c` / `verify_music.py` — `make verify-music`, including a diff against
  atari800's `-pokeyrec` register recording
- `sfx.h` — the original's digitised sound effects (`$3A7C`/`$3AE0`)
- `generated/sfx.h` — the six samples and the tables that fire them
- `verify_sfx.c` — `make verify-sfx`: exercises them and renders each to a WAV
- `generated/frames.h` — the game's animation and dispatch tables (35 moves, 199 frames,
  54 shapes), read out of a RAM dump by `extract_tables.py`
- `generated/shapes_pm.h` — **all 53 fighter poses, in colour**, produced by
  `harvest_v2.sh` + `extract_sprites.py`
- `verify_fighter.c` — `make verify-fighter`: exercises the state machine headlessly
- `generated/scenes.h` — **all seven background stages** (Sydney, New York, Rio de Janeiro,
  Japan, Egypt, Greece, Mount Fuji): the game's own ANTIC mode E bitmaps plus the
  per-scanline colours, recovered by `capture_scenes.sh` + `invert_palette.py`
- `atari_gfx.h` — the mode E playfield renderer
- `hud.h` / `generated/hud.h` — the HUD: the game's own character set, its per-scanline
  colours and its layout, all recovered from the original
- `extract_referee.py` / `generated/referee.h` — the referee
- `extract_hud.py` — regenerates them, and refuses to emit unless re-rendering the two
  mode 4 rows reproduces the capture exactly (`make verify-hud`)
- `verify_scenes.c` / `verify_scenes.py` — `make verify-scenes`: proves the port reproduces
  every stage pixel for pixel
- `game_data.h` — what the generated tables *mean*: the arena bounds, the move-id names and
  the scoring time bonus
- `harvest_v2.sh` / `extract_sprites.py` — the shape-harvesting toolchain
- `check_shape_ids.py` — audits pose labelling against the screen
- `extract_tables.py` / `extract_music.py` — regenerate everything under `generated/`
- `cpu6502.py` — a small 6502 core, kept for analysis
- `SPRITE_AND_MUSIC_INTERNALS.md` — sprite output path and music player, in detail
- `capture_scenes.sh` / `invert_palette.py` — background toolchain: capture each stage from
  the emulator (level loading driven via the monitor), then recover its per-scanline palette
  by inverting the hardware output
- `Makefile`
- `REVERSE_ENGINEERING.md` — recovered algorithm, hardware evidence, sprite and background recovery
- `screenshots/gameplay_rom_logic.png` — the port running
- `screenshots/shape_atlas_colour.png` — all 53 recovered poses
- `screenshots/scene_reconstructed.png` — a stage rebuilt from the recovered data
