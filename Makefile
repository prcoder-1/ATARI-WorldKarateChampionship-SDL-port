CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
PKG     := $(shell pkg-config --cflags --libs sdl2)
LDLIBS  += -lm

worldkarate: worldkarate.c fighter.h ai.h pokey.h sfx.h hud.h atari_gfx.h game_data.h \
             generated/shapes_pm.h generated/scenes.h generated/music.h generated/sfx.h \
             generated/hud.h generated/signs.h generated/pips.h generated/hit.h generated/popup.h generated/hiscore.h hit_test.h hiscore.h
	$(CC) $(CFLAGS) $< -o $@ $(PKG) $(LDLIBS)

run: worldkarate
	./worldkarate

# Check that the port's renderer reproduces the original's backgrounds exactly.
verify-scenes: verify_scenes.c atari_gfx.h generated/scenes.h
	$(CC) $(CFLAGS) -I. verify_scenes.c -o /tmp/verify_scenes $(LDLIBS)
	/tmp/verify_scenes
	python3 verify_scenes.py

# Exercise the ported fighter state machine headlessly, then check the same machine's
# invariants against RAM dumps taken while the real game was fighting.
verify-fighter: verify_fighter.c fighter.h game_data.h
	$(CC) $(CFLAGS) -I. verify_fighter.c -o /tmp/verify_fighter $(LDLIBS)
	/tmp/verify_fighter
	python3 verify_fighter_dumps.py

# Run the music player headlessly and render it to a WAV.
verify-music: verify_music.c pokey.h generated/music.h
	$(CC) $(CFLAGS) -I. verify_music.c -o /tmp/verify_music $(LDLIBS)
	/tmp/verify_music
	python3 verify_music.py

# Drive the ported hit test over every fighter configuration, and replay it against
# RAM dumps taken while the real game was fighting.
verify-hit: verify_hit.c hit_test.h generated/hit.h fighter.h
	$(CC) $(CFLAGS) -I. verify_hit.c -o /tmp/verify_hit $(LDLIBS)
	/tmp/verify_hit

# Render the six digitised sound effects and check them against $3A7C/$3AE0/$399D.
# (An earlier version of this target set out to show the game had no sound effects at
# all. It has six; the scan had only looked at the demo, where they are switched off.)
verify-sfx: verify_sfx.c sfx.h generated/sfx.h
	$(CC) $(CFLAGS) -I. verify_sfx.c -o /tmp/verify_sfx $(LDLIBS)
	/tmp/verify_sfx

# Re-render every extracted pose the way the port draws it and compare with the
# original screenshots, both facings, pixel for pixel.
verify-sprites:
	python3 verify_sprites.py

# The high-score table's text, layout and starting rows, straight from a dump.
verify-hiscore:
	python3 extract_hiscore.py

# The points-scored popup extractor is its own check: it re-draws one back over the
# capture that caught it and refuses to emit unless every ink pixel matches.
verify-popup:
	python3 extract_popup.py

# The marker extractor is its own check as well: it re-draws every dot over each
# capture and refuses to emit unless they match exactly.
verify-pips:
	python3 extract_pips.py

# The sign extractor is its own check too: it re-draws every sign back over each
# capture it came from and refuses to emit unless they match exactly.
verify-signs:
	python3 extract_signs.py

# The HUD extractor is its own check: it refuses to emit unless re-rendering the two
# mode 4 rows reproduces the capture exactly.
verify-hud:
	python3 extract_hud.py

verify: verify-scenes verify-sprites verify-signs verify-pips verify-popup verify-hiscore verify-fighter verify-hit verify-music verify-sfx verify-hud

clean:
	rm -f worldkarate /tmp/verify_scenes /tmp/verify_fighter /tmp/verify_music /tmp/verify_sfx /tmp/verify_hit

.PHONY: run clean verify verify-scenes verify-sprites verify-signs verify-pips verify-popup verify-hiscore verify-fighter verify-hit verify-music verify-sfx verify-hud
