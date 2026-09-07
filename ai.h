/* ai.h - the CPU opponent, ported instruction for instruction from $3D04.
 *
 * The routine runs once per CPU-controlled fighter per frame, immediately before that
 * fighter's state machine ($51C0 sequences: read joysticks, then for each fighter
 * $3D04 followed by $53BC). It decides on a move, writes it to the same queued-move
 * slot the joystick path uses ($EC,y), starts it at once via $530F, then clears the
 * slot again -- so the state machine that follows sees an empty queue.
 *
 * Every branch, comparison and table below is the original's. The tables live in
 * generated/frames.h, read straight out of a RAM dump by extract_tables.py.
 *
 * Two inputs shape almost every decision:
 *   $6136  the gap between the fighters in quarters, from $3F92
 *   $6138  a 3-bit situation code: who is on the right, and both facings
 * and one knob:
 *   $F6    the skill level, 0..5, which selects the RANDOM gates
 *
 * The only thing not carried over literally is POKEY's RANDOM ($D20A) and ANTIC's
 * VCOUNT ($D40B); see aiRandom() / aiVcount().
 */
#ifndef AI_H
#define AI_H

#include "fighter.h"

/* --- the entropy the original uses -------------------------------------------- */
/* $D20A RANDOM: a free-running 17-bit LFSR on the Atari. Any decent 8-bit source
 * gives the same distribution, which is all the tables depend on. */
static unsigned (*aiRandomFn)(void);
static int aiRandom(void) { return (int)(aiRandomFn() & 0xFF); }

/* $D40B VCOUNT counts scanline pairs, 0..$9B, and is read inside $3F71's rejection
 * loop purely as a second entropy source. It advances with the raster, so it differs
 * between the two per-frame calls and between frames; modelled as a free-running
 * counter with the same range. */
static int aiVcountState;
static int aiVcount(void) { aiVcountState = (aiVcountState + 37) % 0x9C; return aiVcountState; }

/* --- $3F71: a random value in [0, limit), by rejection ------------------------- */
static int aiRandLimit(int limit)
{
    for (;;) {
        int a = aiRandom() & 0x1F;
        if (a < limit) return a;
        a = (a & 0x0F) & aiVcount();
        if (a < limit) return a;
    }
}

/* --- $3D04 -------------------------------------------------------------------- */
/* self and opp are the two fighters; skill is $F6. Returns the chosen move, or -1
 * when the routine falls through without deciding. The caller starts the move, which
 * is what $3E32's `JSR $2783` ($530F) does. */
static int aiChooseMove(const Fighter* self, const Fighter* opp, int skill,
                        unsigned (*rndfn)(void))
{
    aiRandomFn = rndfn;
    if (skill < 0) skill = 0;
    if (skill > 5) skill = 5;

    FgtGeom g = fgtGeometry(self, opp);          /* $3F92 -> $6136/$6137/$6138 */
    int gap = g.quarter;                         /* $6136 */
    int sit = g.code & 7;                        /* $6138 */

    /* $3D16: a fighter mid-move only reconsiders on an input-acceptable frame */
    if (self->move != 0 && !(self->attr & ATTR_INPUT)) return -1;

    /* $3D2A: some opponent moves are marked "defer" ($FF) */
    int oppMove = opp->move;
    if (oppMove < 0 || oppMove >= (int)sizeof AI_MOVE_FLAG) oppMove = 0;
    int flag = AI_MOVE_FLAG[oppMove];
    if (flag & 0x80) goto defer;                 /* $3DCA */

    /* $3D39: and so does an opponent who has been holding a pose for a while */
    if (opp->hold >= 8) goto defer;              /* $3DCA */

    /* $3D46 */
    if (gap >= 9) goto idle_gate;                /* $3DDB */

    /* $3D50: a crouching (5) or low-attacking (12) opponent, up close, draws a
     * specific reply chosen by orientation and a random column */
    if (oppMove == 5 || oppMove == 0x0C) {
        if (gap < 8) {
            int a = aiRandom();
            if (a >= AI_RND_CROUCH[skill]) {
                int col = a & 7;                 /* $5D */
                int orient = (sit & 4) ? ((sit & 3) ^ 3) : (sit & 3);
                return AI_ANTI_CROUCH[(orient << 3) + col];
            }
        }
    }

    /* $3D96 */
    if (aiRandom() < AI_RND_CLOSE[skill]) goto idle_gate;   /* $3DDB */
    if (AI_SIT_B[sit] == 0) goto near_far;                  /* $3DF0 */

    /* $3DAE: reply keyed on what the opponent is doing */
    {
        int r = aiRandLimit(8);
        return AI_MOVE_FLAG[oppMove] == 0 ? AI_REPLY0[r] : AI_REPLY1[r];
    }

defer:
    /* $3DCA: only act if standing; and a fighter idle for a long time skips the gate */
    if (self->move != 0) return -1;
    if (self->idle >= 0x30) goto sit_split;      /* $3DE8 */

idle_gate:
    /* $3DDB */
    if (aiRandom() < AI_RND_IDLE[skill]) return -1;

sit_split:
    /* $3DE8 */
    if (AI_SIT_A[sit] == 0) goto by_range;       /* $3E08 */

near_far:
    /* $3DF0 */
    {
        int r = aiRandLimit(12);
        return (gap >= 8) ? AI_FAR[r] : AI_NEAR[r];
    }

by_range:
    /* $3E08 */
    if (gap >= 0x0A) {
        int r = aiRandLimit(12);
        return AI_MID[r];
    }
    /* $3E1A: a row per range bracket, a random column per skill */
    {
        int row = AI_ROW[gap];                   /* $3F66 */
        int r = aiRandLimit(AI_COLS[skill]);
        return AI_BY_RANGE[row + r];
    }
}

#endif
