/* atari_gfx.h - render the game's own background the way ANTIC/GTIA did.
 *
 * The scene is 97 scanlines of ANTIC mode E: 160 pixels per line, 2 bits per pixel.
 * A mode-E pixel is two colour clocks wide, and the port's 320-px logical screen is
 * one pixel per colour clock, so a scene pixel is drawn 2 logical px wide and the
 * playfield fills the width exactly.
 *
 * Colours come from generated/scenes.h, recovered by inverting the real hardware
 * output (invert_palette.py). Values 1..3 are constant per scanline; value 0 selects
 * COLBK, which the original rewrites mid-scanline to paint horizontal bands, so it is
 * looked up from that line's run list.
 *
 * Vertical placement matches the original: the playfield starts at scanline 31 (13
 * blank + two mode-4 HUD rows + 2 blank, per the display list at $6260), and the solid
 * ground band fills everything below it.
 */
#ifndef ATARI_GFX_H
#define ATARI_GFX_H

#include "generated/scenes.h"

#define SCENE_TOP 31   /* first playfield scanline, from the display list */
#define SCENE_LEFT 32  /* first playfield colour clock (standard playfield origin) */
#define SCENE_RIGHT (SCENE_LEFT + SCENE_PIXELS * CLOCKS_PER_SCENE_PX)
#define CLOCKS_PER_SCENE_PX 2

/* setpx(x, y, r, g, b) is supplied by the caller (worldkarate.c) */
static void sceneBorder(const BgScene* s, int screenW, int screenH,
                        void (*setpx)(int, int, int, int, int));
static void sceneDraw(const BgScene* s, int screenW, int screenH,
                      void (*setpx)(int, int, int, int, int)) {
    sceneBorder(s, screenW, screenH, setpx);
    for (int line = 0; line < SCENE_LINES; line++) {
        int y = SCENE_TOP + line;
        if (y < 0 || y >= screenH) continue;
        const uint8_t* row = s->bits + line * SCENE_BYTES_PER_LINE;
        const SceneRun* runs = s->runs + s->run_ofs[line];
        int nruns = (int)(s->run_ofs[line + 1] - s->run_ofs[line]);
        int ri = 0;
        SceneCol bk = s->ground;                 /* until the first run starts */
        for (int p = 0; p < SCENE_PIXELS; p++) {
            while (ri < nruns && runs[ri].x <= p) {
                bk = s->pal[runs[ri].col];
                ri++;
            }
            int v = (row[p >> 2] >> (6 - 2 * (p & 3))) & 3;
            SceneCol c = v ? s->pal[s->fixed[line][v - 1]] : bk;
            int x = SCENE_LEFT + p * CLOCKS_PER_SCENE_PX;
            if (x + 1 < screenW) {
                setpx(x,     y, c.r, c.g, c.b);
                setpx(x + 1, y, c.r, c.g, c.b);
            }
        }
    }
}

static void sceneBorder(const BgScene* s, int screenW, int screenH,
                        void (*setpx)(int, int, int, int, int)) {
    /* ground below the playfield, black border around it, as on the Atari */
    for (int y = 0; y < screenH; y++) {
        int below = y >= SCENE_TOP + SCENE_LINES;
        for (int x = 0; x < screenW; x++) {
            int inside = x >= SCENE_LEFT && x < SCENE_RIGHT;
            if (y < SCENE_TOP + SCENE_LINES && inside) continue;   /* playfield, drawn above */
            if (below && inside) setpx(x, y, s->ground.r, s->ground.g, s->ground.b);
            else                 setpx(x, y, 0, 0, 0);
        }
    }
}

#endif
