/* fighter.h - the original's fighter state machine, ported from the 6502.
 *
 * The port used to invent its own move table, durations and jump physics. This is the
 * game's own logic instead, driven entirely by the tables in game_data.h.
 *
 * The central idea recovered from the ROM ($53BC, $530F, $5509, $51F9):
 *
 *   A fighter's state IS an index into the per-frame tables.
 *
 * $6181,y holds the move id and $EE,y the animation frame index. Each game tick the
 * index advances by one; when it reaches MOVE_FRAME_START[move+1] the move is over and
 * the queued move is taken. Per frame, FRAME_ATTR says what may happen, FRAME_VELX how
 * far the fighter slides, and FRAME_SHAPE which pose to draw.
 *
 * Three consequences worth stating, because they are what made the port feel wrong:
 *
 *   - There is NO vertical motion and no gravity. $5509 only ever touches $E0/$E1, the
 *     horizontal position. Jumps, somersaults and falls are drawn into the poses; each
 *     pose carries its own absolute top scanline (ShapePM.y0).
 *   - Move duration is not tunable: it is exactly the length of the move's frame range.
 *   - **Half of this routine belongs to joystick-controlled fighters only.** $53D6 and
 *     $543F both test $0050,y and jump over the whole block when it is zero, so a CPU
 *     fighter never reverses a held pose, never stalls on an ATTR_HOLD frame, and never
 *     re-dispatches on an ATTR_INPUT frame. It plays each move it starts straight
 *     through. Missing that gate is what made the CPU opponent's movement break up: it
 *     unwound poses backwards and restarted moves partway.
 *     Confirmed against the machine: over 30 RAM dumps taken during a fight, in all 60
 *     samples of a CPU-controlled fighter $6187 (reverse) and $6185 (hold) are zero.
 *
 * Fighter x is the sprite's LEFT EDGE in sprite-pixel units, an 8-bit quantity clamped
 * by $553E/$5564 so the figure stays inside the arena $10..$AE.
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
#define MOVE_BOW       0x1C   /* 28: the ceremony; $3981 stops the clock during it */

typedef struct {
    int x;          /* $E0,y  left edge, arena units, 8-bit         */
    int facing;     /* $E2,y  0 = faces right, 1 = faces left       */
    int flipmark;   /* $E4,y  set when ATTR_FLIP flipped the facing */
    int move;       /* $6181,y                                      */
    int frame;      /* $EE,y  index into the per-frame tables       */
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
    int index;      /* $D2      which fighter this is, 0 or 1       */
    /* port-side extra, not part of the ROM state. The points, the score and the win
     * count are NOT here: they outlive a bout, and this struct is wiped whenever the
     * fighters are placed. */
    int isCPU;
} Fighter;

/* $6114: the fighter an ATTR_TURN frame handed the turn to. $3C2E reads it to order the
 * pair -- $611C takes $EA[owner] and $611D $EA[the other], and $EA holds the two figures'
 * colours ($0F and $36, set at $32C8) -- so the owner is the one composed first. */
static int fgtTurnOwner;

/* --- $530F: choose the next move ------------------------------------------------ */
static void fgtStartMove(Fighter* f, int forced, unsigned (*rnd)(void))
{
    int m;
    if (forced >= 0) {                     /* $5313: play frozen, $6131 is imposed */
        m = forced;
        f->queued = m;                     /* it goes through $EC like any other   */
        f->idle = m;
    } else if (f->queued) {                /* $531D -> $535A */
        m = f->queued;
        f->idle = m;                       /* $535A stores the move into $618B too */
    } else {
        f->idle = (f->idle + 1) & 0xFF;    /* $5324 */
        if (f->idle >= 0x80 && (rnd() & 0xFF) < 0x10) {
            m = MOVE_IDLE;                 /* $5337 */
            f->idle = 0x10 + (rnd() & 0x3F);
        } else {
            m = 0;
        }
    }
    if (m < 0 || m >= NUM_MOVE_IDS) m = 0;
    f->move = m;                                   /* $535D */
    f->frame = f->next = MOVE_FRAME_START[m];      /* $5361 */
    f->samemove = 1;                               /* $536A */
    f->reverse = f->recovery = f->hold = 0;        /* $536F */
    f->flipmark = 0;
    f->attr = f->prevattr = 0;                     /* $537D */
}

/* --- $5509/$550E: apply this frame's horizontal velocity and clamp to the arena ---
 * back is the direction flag the ROM branches on at $550E: nonzero subtracts. On the
 * normal path it is the facing ($550B); on the reverse path it is reverse EOR facing
 * ($54FF), so an unwinding pose slides back the way it came. */
static void fgtApplyVelDir(Fighter* f, int back)
{
    int v = FRAME_VELX[f->frame];
    int w = SHAPE_WIDTH[f->shape % NUM_SHAPES];
    int left, right;

    if (back) {                                    /* $5510 */
        f->x = (f->x - v) & 0xFF;
        right = f->x + 0x24;                       /* $551D */
        left  = right - w;                         /* $5522 */
    } else {                                       /* $552A */
        f->x = (f->x + v) & 0xFF;
        left  = f->x;
        right = left + w;                          /* $5539 */
    }

    if (ARENA_LEFT >= left) {                      /* $553E: CMP #$10 / BCC $5564 */
        if (!f->facing) {
            f->x = ARENA_LEFT;                     /* $5549 */
        } else {
            int t = 0x12 + w - 0x24;               /* $5551, and note the $12 */
            f->x = t < 0 ? 0 : t;                  /* $555A: BCS / LDA #$00    */
        }
    } else if (ARENA_RIGHT < right) {              /* $5564: CMP #$AE / BCS $5580 */
        if (!f->facing) f->x = ARENA_RIGHT - w;    /* $556F */
        else            f->x = 0x8A;               /* $557B */
    }
    if (f->x >= 0xF0) f->x = 0;                    /* $5580: the underflow guard */
}

/* --- $53BC: one tick of a fighter ------------------------------------------------
 * A straight transcription, labels and all, because the control flow matters: $546A
 * and $54AD both re-enter the routine partway, and the reverse path at $54E5 falls
 * back into it through $5407 rather than returning. */
static void fgtUpdate(Fighter* f, int freezeMove, unsigned (*rnd)(void))
{
    int guard, spins;

    if (f->locked) return;                                   /* $53BE -> $54E4 */

    for (spins = 0; spins < 8; spins++) {
    /* L53C6 */
        f->facing &= 1;                                      /* $53C6 */
        f->isHuman &= 1;                                     /* $53CE */
        if (f->isHuman) {                                    /* $53D6: CPU skips it all */
            if (f->queued != f->move) f->samemove = 0;       /* $53D8 */
            if (!f->samemove && f->recovery) f->reverse = 1; /* $53E5 */
            if (f->reverse) {                                /* $53F4 -> $54E5 */
                f->frame--;                                  /* $54E5 */
                if (f->frame >= MOVE_FRAME_START[f->move]) { /* $54EF: CMP $558D,x */
                    f->shape = FRAME_SHAPE[f->frame];        /* $54F7, no $36 guard */
                    fgtApplyVelDir(f, 1 ^ (f->facing & 1));  /* $54FF */
                    return;                                  /* $5508 */
                }
                goto L5407;                                  /* $54F4 */
            }
        }

        f->frame = f->next;                                  /* $53FC */
        if (f->move) goto L540A;                             /* $5402 */
    L5407:
        fgtStartMove(f, freezeMove, rnd);                    /* $5407: JSR $530F */
    L540A:
        /* $540A: past the end of this move's range -> take the next one */
        for (guard = 0; guard < 16 && f->frame >= MOVE_FRAME_START[f->move + 1]; guard++)
            fgtStartMove(f, freezeMove, rnd);                /* $5415 -> $5407 */
        if (f->frame < 0 || f->frame >= NUM_FRAMES) f->frame = MOVE_FRAME_START[0];

        f->shape = FRAME_SHAPE[f->frame];                    /* $5418 */
        if (f->shape >= 0x36) f->shape = 0;                  /* $541D */
        f->prevattr = f->attr;                               /* $5426 */
        f->attr = FRAME_ATTR[f->frame];                      /* $542C */
        f->recovery = f->attr & ATTR_RECOVERY;               /* $5432 */
        f->gate = f->attr & ATTR_GATE;                       /* $5437 */

        if (!f->isHuman) break;                              /* $543F -> $54B0 */

        if (f->attr & ATTR_HOLD) {                           /* $5444 */
            if (f->queued == f->move) {                      /* $544B -> $546D */
                if (f->hold & 0x80) return;                  /* $5470 */
                if (f->hold == 0) fgtApplyVelDir(f, f->facing);  /* $5474 */
                f->hold = (f->hold + 1) & 0xFF;              /* $5479 */
                return;                                      /* $547C */
            }
            if (f->hold) {                                   /* $5453: released */
                f->hold = 0;                                 /* $5458 */
                f->reverse = 0;
                f->recovery = 0;
                f->next = f->frame + 1;                      /* $5463 */
                continue;                                    /* $546A: JMP $53C6 */
            }
        }

        if ((f->attr & ATTR_INPUT) && !f->samemove) {        /* $547F/$5486 */
            if (f->move == AM_WALK_F) goto L5407;            /* $5490 -> $549B */
            if (f->move == AM_WALK_B) {                      /* $5492 -> $549E */
                f->x = (f->x + (f->facing ? 3 : -3)) & 0xFF; /* the back-step */
                goto L5407;                                  /* $54AD */
            }
            if (f->queued) goto L5407;                       /* $5496 -> $549B */
        }
        break;
    }

    if (f->attr & ATTR_FLIP) {                               /* $54B0 */
        f->facing ^= 1;
        f->flipmark = 1;
    }
    if (f->attr & ATTR_TURN) fgtTurnOwner = f->index;        /* $54C4: STY $6114 */
    if (f->attr & ATTR_LOCK) f->locked = 1;                  /* $54CE */
    fgtApplyVelDir(f, f->facing);                            /* $54DA */
    f->next = f->frame + 1;                                  /* $54DD */
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

#endif
