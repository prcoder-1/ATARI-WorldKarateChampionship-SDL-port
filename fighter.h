/* fighter.h - the original's fighter state machine, ported from the 6502.
 *
 * The port used to invent its own move table, durations and jump physics. This is the
 * game's own logic instead, driven entirely by the tables in game_data.h.
 *
 * The central idea recovered from the ROM ($53BC, $530F, $5509, $51F9):
 *
 *   A fighter's state IS an index into the 120-entry per-frame tables.
 *
 * $6181,y holds the move id and $EE,y the animation frame index. Each video frame the
 * index advances by one; when it reaches MOVE_FRAME_START[move+1] the move is over and
 * the queued move is taken. Per frame, FRAME_ATTR says what may happen, FRAME_VELX how
 * far the fighter slides, and FRAME_SHAPE which pose to draw.
 *
 * Two consequences worth stating, because they are what made the old port feel wrong:
 *
 *   - There is NO vertical motion and no gravity. $5509 only ever touches $E0/$E1, the
 *     horizontal position. Jumps, somersaults and falls are drawn into the poses; each
 *     pose carries its own absolute top scanline (ShapePM.y0).
 *   - Move duration is not tunable: it is exactly the length of the move's frame range.
 *
 * Fighter x is the sprite's LEFT EDGE in sprite-pixel units, clamped so that
 * [x, x + SHAPE_WIDTH[shape]] stays inside the arena $10..$AE.
 */
#ifndef FIGHTER_H
#define FIGHTER_H

#include <stdint.h>
#include "game_data.h"

/* FRAME_ATTR bits, from $5423-$54DA */
#define ATTR_TURN      0x01   /* $6114: this fighter becomes the "turn" owner */
#define ATTR_FLIP      0x02   /* flip facing, and set the flip marker $E4      */
#define ATTR_RECOVERY  0x04   /* $6189: a held pose that unwinds when released */
#define ATTR_HOLD      0x08   /* frame may be HELD while the same move is asked   */
#define ATTR_INPUT     0x10   /* input may be taken on this frame              */
#define ATTR_LOCK      0x20   /* $618D: fighter is finished (knocked out)      */
#define ATTR_GATE      0x40   /* $618F: opens the opponent's alternate moves   */

#define MOVE_IDLE      0x15   /* $5337 picks move 21 when a fighter idles long */
/* Move ids are 0..NUM_MOVE_IDS-1; MOVE_FRAME_START has NUM_MOVE_IDS+1 entries so
 * MOVE_FRAME_START[m+1] is always in range. */
#define MOVE_HIT       0x11   /* 17: struck from the front                     */
#define MOVE_FALL      0x12   /* 18: struck from behind                        */

typedef struct {
    int x;          /* $E0,y  left edge, arena units                */
    int facing;     /* $E2,y  0 = faces right, 1 = faces left       */
    int flipmark;   /* $E4,y  set when ATTR_FLIP flipped the facing */
    int move;       /* $6181,y                                      */
    int frame;      /* $EE,y  index into the 120-entry tables       */
    int next;       /* $F0,y                                        */
    int queued;     /* $EC,y  move selected by input or the AI      */
    int shape;      /* $DD,y                                        */
    int attr;       /* $6191,y                                      */
    int prevattr;   /* $6193,y                                      */
    int samemove;   /* $6183,y  queued move still equals current    */
    int recovery;   /* $6189,y                                      */
    int gate;       /* $618F,y                                      */
    int reverse;    /* $6187,y  play the animation backwards        */
    int locked;     /* $618D,y                                      */
    int idle;       /* $618B,y  idle counter feeding the taunt      */
    int hold;       /* $6185,y  frames spent held on an ATTR_HOLD frame */
    int isHuman;    /* $0050,y  nonzero = joystick, zero = the CPU routine */
    /* port-side extras, not part of the ROM state */
    int isCPU;
    int points;
    int score;      /* the HUD's running score */
    int wins;
} Fighter;

/* --- $530F: choose the next move ------------------------------------------------ */
static void fgtStartMove(Fighter* f, int forced, unsigned (*rnd)(void))
{
    int m;
    if (forced >= 0) {
        m = forced;
    } else if (f->queued) {
        m = f->queued;
        f->idle = m;                       /* $535A also re-arms the idle counter */
    } else {
        f->idle = (f->idle + 1) & 0xFF;
        if (f->idle >= 0x80 && (rnd() & 0xFF) < 0x10) {
            m = MOVE_IDLE;                 /* $5337 */
            f->idle = 0x10 + (rnd() & 0x3F);
        } else {
            m = 0;
        }
    }
    if (m < 0 || m >= NUM_MOVE_IDS) m = 0;
    f->move = m;
    f->frame = f->next = MOVE_FRAME_START[m];
    f->samemove = 1;
    f->reverse = f->recovery = f->flipmark = 0;
    f->hold = 0;
    f->attr = f->prevattr = 0;
}

/* --- $5509: apply this frame's horizontal velocity and clamp to the arena -------- */
static void fgtApplyVel(Fighter* f)
{
    int v = FRAME_VELX[f->frame];
    int w = SHAPE_WIDTH[f->shape % NUM_SHAPES];
    int left, right;
    f->x += f->facing ? -v : v;
    if (f->facing) { right = f->x + 0x24; left = right - w; }
    else           { left  = f->x;        right = left + w; }
    if (left  < ARENA_LEFT)  f->x += ARENA_LEFT  - left;
    if (right > ARENA_RIGHT) f->x -= right - ARENA_RIGHT;
}

/* --- $54E5: the reverse-playback path (a held pose unwinding) -------------------- */
static void fgtReverse(Fighter* f, int freezeMove, unsigned (*rnd)(void))
{
    f->frame--;
    if (f->frame < MOVE_FRAME_START[f->move]) {
        f->reverse = 0;
        fgtStartMove(f, freezeMove, rnd);
        return;
    }
    f->shape = FRAME_SHAPE[f->frame];
    if (f->shape >= 0x36) f->shape = 0;
    fgtApplyVel(f);
    f->next = f->frame;
}

/* --- $53BC: one frame of a fighter ----------------------------------------------- */
static void fgtUpdate(Fighter* f, int freezeMove, unsigned (*rnd)(void))
{
    int guard, restarts;
    if (f->locked) return;

    for (restarts = 0; restarts < 4; restarts++) {
        f->facing &= 1;

        if (f->queued != f->move) f->samemove = 0;      /* $53C6 */
        if (!f->samemove && f->recovery) f->reverse = 1;/* $53E5 */
        if (f->reverse) { fgtReverse(f, freezeMove, rnd); return; }  /* $53F4 */

        f->frame = f->next;                             /* $53FC */
        if (!f->move) fgtStartMove(f, freezeMove, rnd);
        /* $540A: past the end of this move's frame range -> take the next move */
        for (guard = 0; f->frame >= MOVE_FRAME_START[f->move + 1] && guard < 8; guard++) {
            fgtStartMove(f, freezeMove, rnd);
            f->frame = f->next;
        }
        if (f->frame < 0 || f->frame >= NUM_FRAMES) f->frame = MOVE_FRAME_START[0];

        f->shape = FRAME_SHAPE[f->frame];               /* $5418 */
        if (f->shape >= 0x36) f->shape = 0;
        f->prevattr = f->attr;
        f->attr = FRAME_ATTR[f->frame];
        f->recovery = f->attr & ATTR_RECOVERY;
        f->gate = f->attr & ATTR_GATE;

        /* $5440: the hold branch, which the ROM runs only for a joystick-controlled
         * fighter -- a CPU fighter never stalls on an ATTR_HOLD frame. */
        if (f->isHuman && (f->attr & ATTR_HOLD)) {
            if (f->queued == f->move) {                 /* $546D */
                if (f->hold & 0x80) return;
                if (f->hold == 0) fgtApplyVel(f);       /* only on the first held frame */
                f->hold++;
                return;
            }
            if (f->hold) {                              /* $5450: released */
                f->hold = 0;
                f->reverse = 0;
                f->recovery = 0;
                f->next = f->frame + 1;
                continue;                               /* $53C6: run the frame again */
            }
        }
        break;
    }

    if ((f->attr & ATTR_INPUT) && !f->samemove) {   /* $547F */
        if (f->move == AM_WALK_F) {
            fgtStartMove(f, freezeMove, rnd); f->frame = f->next;
        } else if (f->move == AM_WALK_B) {
            f->x += f->facing ? 3 : -3;             /* $549E: the back-step */
            fgtStartMove(f, freezeMove, rnd); f->frame = f->next;
        } else if (f->queued) {
            fgtStartMove(f, freezeMove, rnd); f->frame = f->next;
        }
        f->shape = FRAME_SHAPE[f->frame];
        if (f->shape >= 0x36) f->shape = 0;
        f->attr = FRAME_ATTR[f->frame];
    }

    if (f->attr & ATTR_FLIP) {                      /* $54B0 */
        f->facing ^= 1;
        f->flipmark = 1;
    }
    if (f->attr & ATTR_LOCK) f->locked = 1;
    fgtApplyVel(f);
    f->next = f->frame + 1;
}

/* --- $51F9: joystick (or AI) direction+fire -> queued move ----------------------- */
/* stick is an Atari direction nibble, ACTIVE LOW: bit0 up, 1 down, 2 left, 3 right. */
static int fgtDispatch(const Fighter* f, int stick, int fire, int alt)
{
    int idx = ((fire ? 0x10 : 0) | (stick & 0x0F));
    int sel = (f->facing ^ f->flipmark) & 1;
    if (alt) return sel ? DISPATCH_altL[idx] : DISPATCH_altR[idx];
    return sel ? DISPATCH_L[idx] : DISPATCH_R[idx];
}

/* --- $3F92: the geometry both the AI and the alternate-move gate run on ---------- */
typedef struct { int gap; int quarter; int code; } FgtGeom;

static FgtGeom fgtGeometry(const Fighter* a, const Fighter* b)
{
    FgtGeom g;
    int pa = a->facing ? a->x + 0x24 - 0x0C : a->x + 0x0C;
    int pb = b->facing ? b->x + 0x24 - 0x0C : b->x + 0x0C;
    int d = pa - pb;
    g.code = ((((d >= 0) ? 1 : 0) << 1 | (a->facing & 1)) << 1) | (b->facing & 1);
    g.gap = d < 0 ? -d : d;
    g.quarter = g.gap >> 2;
    return g;
}

static int am_is_attack_move(int m) { return am_is_attack(m); }

#endif
