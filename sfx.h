/* sfx.h - the game's sound effects, ported from $3A7C / $3AE0 / $399D.
 *
 * They are digitised 4-bit samples, not tones. Starting one ($3A7C) points a cursor at
 * the sample data, sets AUDF1 = $10, zeroes the rest of POKEY and enables the timer-1
 * interrupt; the handler ($3AE0) fires at about 3760 Hz and writes one nibble to AUDC1,
 * AUDC2 and AUDC3 with bit 4 set, POKEY's volume-only mode, so the nibble is the
 * speaker level on three channels at once.
 *
 * Starting an effect also sets $CF ($27E3), which is the music player's mute flag: the
 * player keeps running its state but stops copying its shadow to POKEY. Clearing $CF
 * when the effect ends ($27E8) brings the music back. That is why an effect interrupts
 * the music rather than mixing with it, and it is reproduced here.
 *
 * Triggering ($399D): each frame, for each fighter, if its shape id changed, look the
 * new shape up in an 11-entry table and pick an effect according to the game state:
 * fighting, fighting with the referee freezing play, or state 2/5. Some entries also
 * arm a delayed second effect ($617C/$617D, counted down in the vertical blank).
 *
 * Two enable flags, both toggled from the keyboard:
 *   $0054  music        $0055  sound effects
 * The attract demo runs with music on and effects off; starting a game turns effects on
 * and the music off ($3337). Either can then be toggled back.
 */
#ifndef SFX_H
#define SFX_H

#include <stdint.h>
#include "generated/sfx.h"

typedef struct {
    int active;
    int cursor;       /* $3B1A/$3B1B: index into SFX_DATA          */
    int end;          /* the effect's end offset                   */
    int high;         /* which nibble ($3B1C-$3B1F, self-modified) */
    double phase;     /* host-rate resampling accumulator          */
    int level;        /* the nibble currently on the speaker       */
    /* $617C/$617D: a delayed second effect */
    int delay;
    int delayed;
    /* per-fighter shape memory, $6173/$6174 */
    int lastShape[2];
} Sfx;

static void sfxInit(Sfx* s)
{
    s->active = 0; s->cursor = s->end = s->high = 0;
    s->phase = 0.0; s->level = 8;
    s->delay = 0; s->delayed = 0;
    s->lastShape[0] = s->lastShape[1] = -1;
}

/* $3A7C. enabled is $0055; returns nonzero if the effect actually started. */
static int sfxStart(Sfx* s, int n, int enabled)
{
    if (n < 0 || n >= SFX_COUNT) return 0;      /* $3A7C: CPX #$06 / BCS */
    if (!enabled) return 0;                     /* $3A80: LDA $55 / BEQ  */
    s->cursor = SFX_START[n];
    s->end = SFX_END[n];
    s->high = SFX_HIGH_NIBBLE[n];
    s->phase = 0.0;
    s->active = 1;
    return 1;
}

/* $3AE0: advance by one interrupt. Returns 0 when the effect has finished. */
static int sfxStep(Sfx* s)
{
    if (!s->active) return 0;
    if (s->cursor >= s->end) { s->active = 0; return 0; }
    {
        uint8_t b = SFX_DATA[s->cursor++];
        s->level = s->high ? (b >> 4) : (b & 0x0F);
    }
    return 1;
}

/* $399D + $3A16: watch a fighter's shape and queue the effect its change calls for.
 *   shape   the fighter's new shape id
 *   attr    that frame's attribute bits ($6191,y)
 *   state   the game state ($00D0): 1 = fighting, 2 and 5 = the other sounding states
 *   frozen  the referee freeze flag ($00D8)
 * Returns the effect number, or -1. */
static int sfxForShapeChange(Sfx* s, int shape, int attr, int state, int frozen)
{
    int i;
    for (i = 0; i < SFX_EVENTS; i++)
        if (SFX_EVENT_SHAPE[i] == shape) break;
    if (i == SFX_EVENTS) return -1;             /* $3A26: no match */

    if (state == 1) {
        if (!frozen) {                          /* $3A2E */
            if (!(attr & SFX_ATTR_MASK)) return -1;
            return (int8_t)SFX_WHILE_FIGHTING[i] < 0 ? -1 : SFX_WHILE_FIGHTING[i];
        }
        if (SFX_DELAY[i]) {                     /* $3A3F: arm the second one */
            s->delay = SFX_DELAY[i];
            s->delayed = SFX_DELAYED[i];
        }
        return (int8_t)SFX_WHILE_FROZEN[i] < 0 ? -1 : SFX_WHILE_FROZEN[i];
    }
    if (state == 2 || state == 5)               /* $3A53 */
        return (int8_t)SFX_STATE_2_5[i] < 0 ? -1 : SFX_STATE_2_5[i];
    return -1;
}

/* One video frame: run the shape watcher for both fighters and the delay countdown.
 * shapes[] and attrs[] are indexed by fighter, as $DD,y and $6191,y are. */
static void sfxFrame(Sfx* s, const int shapes[2], const int attrs[2],
                     int state, int frozen, int enabled)
{
    int want = -1, y;
    /* $39AB counts down from fighter 1 to fighter 0, and the last match wins */
    for (y = 1; y >= 0; y--) {
        if (shapes[y] == s->lastShape[y]) continue;
        s->lastShape[y] = shapes[y];
        {
            int e = sfxForShapeChange(s, shapes[y], attrs[y], state, frozen);
            if (e >= 0) want = e;
        }
    }
    if (s->delay) {                             /* $395F, in the vertical blank */
        if (--s->delay == 0) sfxStart(s, s->delayed, enabled);
    }
    if (want >= 0) sfxStart(s, want, enabled);
}

/* Render n samples, replacing the buffer. The three volume-only channels sum, and the
 * samples sit around mid-scale, so the DC offset is removed. */
static void sfxRender(Sfx* s, int16_t* out, int n, int rate)
{
    double step = SFX_RATE / rate;
    int i;
    for (i = 0; i < n; i++) {
        s->phase += step;
        while (s->phase >= 1.0) {
            s->phase -= 1.0;
            if (!sfxStep(s)) break;
        }
        out[i] = (int16_t)((s->level - 8) * 3 * 260);
        if (!s->active) { out[i] = 0; }
    }
}

static int sfxActive(const Sfx* s) { return s->active; }

#endif
