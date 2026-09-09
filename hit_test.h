/* hit_test.h - the original's hit test, transcribed from $415D.
 *
 * The port used to decide a blow had landed when an attacking fighter's sprite
 * overlapped the opponent's. The game does nothing of the kind. It never looks at
 * pixels, and it never asks the hardware: the one GTIA collision register the game
 * reads at all ($D004, at $388C) feeds $6155, which belongs to the referee's own loop,
 * not to this.
 *
 * $415D is geometry between points, run once per game tick from the fight loop
 * ($288A: JSR $2729). In outline:
 *
 *   1. Find the attacker. A blow can only be thrown on a tick where a fighter's DRAWN
 *      SHAPE is one of the eight in $400B -- not its move, not its frame -- and only if
 *      that pose has not been held for two ticks or more ($6185 >= 2, $4185). Both
 *      fighters are tried, starting with the one $6115's parity names.
 *   2. The strike point is the attacker's x plus $4014[strike], mirrored inside the $24
 *      box when it faces left ($41E4).
 *   3. The defender's shape maps through $4082 to a target class. $80 or more and it
 *      cannot be hit at all -- 23 of the 54 shapes are like that, which is how a fighter
 *      already falling cannot be hit again.
 *   4. Two classes are adjusted: $18 becomes $1A against strike 3 ($41A2), and $13
 *      collapses to 0 when the two fighters' situation code is 0 or 7 ($41BB).
 *   5. Each class has six body points, $40BB[class*6 + part], each an offset from the
 *      defender's x, again $80 or more for a part the class does not have. A strike only
 *      reaches some of them: $405C[part] AND $401C[strike] ($4203).
 *   6. The distance between the two points is formed one of four ways by the pair of
 *      facings ($6138 AND 3, $4244), and compared with the strike's own limits: under
 *      $402C the blow is solid and scores two, between $402C and $4024 it is glancing
 *      and scores one, past $4024 or negative it misses and the next part is tried.
 *
 * All the arithmetic is 8-bit, and $4286's BPL is a test of bit 7, not of a signed int:
 * a distance of $F8 is "negative" and misses.
 *
 * The tables are generated into generated/hit.h. Nothing here is invented; where the
 * ROM's intent is not clear the value is carried through rather than interpreted --
 * $613F (which announcement) and $6140 (which column) are recorded and not yet used,
 * because what they index has not been established.
 */
#ifndef HIT_TEST_H
#define HIT_TEST_H

#include <string.h>
#include "fighter.h"
#include "generated/hit.h"

typedef struct {
    int hit;        /* $00D8: 0 nothing, 1 a glancing blow, 2 a solid one */
    int attacker;   /* $D4    */
    int defender;   /* $6128  */
    int strike;     /* $D5    */
    int part;       /* $D6    */
    int distance;   /* $612A  */
    int score;      /* $6132, BCD, added to the fighter's score at $4550 */
    int sign;       /* $613F, recorded; what it indexes is not established */
    int column;     /* $6140, likewise */
    int reaction;   /* $6130, the move forced on the defender */
} HitResult;

/* $4062 is indexed by strike*4 + the facing pair, and $4082 begins four bytes into
 * that table's last row -- the ninth strike is the $FF sentinel and never occurs, so
 * the two tables share those bytes in the ROM. Guard the index rather than rely on it. */
static int hitReaction(int strike, int facing_pair)
{
    int i = strike * 4 + facing_pair;
    return i < (int)sizeof HIT_REACTION ? HIT_REACTION[i] : 0;
}

/* One tick of $415D. `parity` is $6115; f[0] and f[1] are the two fighters. */
static HitResult hitTest(const Fighter* f[2], int parity)
{
    HitResult r;
    int x, guard, strike, s;

    memset(&r, 0, sizeof r);

    /* $4165: two attempts, the parity fighter first ($5D counts them down) */
    x = parity & 1;
    guard = 1;
    strike = -1;
    for (;;) {
        for (s = HIT_STRIKES - 1; s >= 0; s--)          /* $416A: LDY #$08, DEY/BPL */
            if (f[x]->shape == HIT_STRIKE_SHAPE[s]) break;
        /* $4185: a held pose has spent its blow */
        if (s >= 0 && f[x]->hold < HIT_HOLD_LIMIT) { strike = s; break; }
        x ^= 1;                                          /* $4176 */
        if (--guard < 0) return r;                       /* $417A/$417E */
    }

    {
        const Fighter* a = f[x];                         /* $D4 */
        const Fighter* b = f[x ^ 1];                     /* $6128 */
        int cls, pair, situation, far, near, strikePoint, part;

        r.attacker = x;
        r.defender = x ^ 1;
        r.strike = strike;

        cls = HIT_SHAPE_CLASS[b->shape % HIT_SHAPES];    /* $4197 */
        if (cls >= HIT_ABSENT) return r;                 /* $419D: cannot be hit */
        if (cls == HIT_CLASS_SPECIAL && strike == HIT_CLASS_SPECIAL_STRIKE)
            cls = HIT_CLASS_SPECIAL_TO;                  /* $41A2 */

        /* $41B3: JSR $3F92 leaves the situation code in A; $41B6 keeps the two facings */
        situation = fgtGeometry(a, b).code & 7;
        pair = situation & 3;
        if (cls == HIT_CLASS_FLAT && (situation == 0 || situation == 7))
            cls = 0;                                     /* $41BB */
        if (cls >= HIT_CLASSES) return r;

        far  = HIT_FAR[strike];                          /* $4024 */
        near = HIT_NEAR[strike];                         /* $402C */

        /* $41E4: the strike point, mirrored in the $24 box when facing left */
        strikePoint = a->facing ? (a->x + 0x24 - HIT_STRIKE_POINT[strike])
                                : (a->x + HIT_STRIKE_POINT[strike]);
        strikePoint &= 0xFF;

        for (part = 0; part < HIT_PARTS; part++) {       /* $4201..$4306 */
            int off, target, d;

            if (!(HIT_PART_BIT[part] & HIT_STRIKE_PARTS[strike]))
                continue;                                /* $4203: out of this strike's reach */

            off = HIT_PART_OFFSET[cls * HIT_PARTS + part];   /* $4220 */
            if (off >= HIT_ABSENT) continue;             /* the class has no such part */

            target = b->facing ? (b->x + 0x24 - off) : (b->x + off);   /* $422A */
            target &= 0xFF;

            switch (pair) {                              /* $4244 */
            case 0:  d = strikePoint - target + far; break;
            case 1:  d = strikePoint - target;       break;
            case 2:  d = target - strikePoint;       break;
            default: d = target - strikePoint + far; break;
            }
            d &= 0xFF;                                   /* $4283: it is one byte */

            if (d & 0x80) continue;                      /* $4286: BPL */
            if (d > far) continue;                       /* $4290: BCS, d != far */
            /* $4292: under the near limit it is solid, otherwise glancing */
            r.hit = (d < near) ? 2 : 1;
            r.part = part;
            r.distance = d;
            r.score = (r.hit == 2) ? HIT_SCORE_SOLID[strike] : HIT_SCORE_GLANCE[strike];
            r.sign  = (r.hit == 2) ? HIT_SIGN_SOLID[strike]  : HIT_SIGN_GLANCE[strike];
            r.reaction = hitReaction(strike, pair);      /* $42D8 */
            /* $42DE: where the announcement goes, the attacker's x in character cells */
            {
                int col = ((a->x >> 2) + HIT_COL_BIAS[pair]) & 0xFF;
                if (col > HIT_COL_MAX) col = HIT_COL_MAX;
                if (col < HIT_COL_MIN) col = HIT_COL_MIN;
                r.column = col;
            }
            return r;
        }
    }
    return r;
}

/* $4502: $00D9,y accumulates the blows, a solid one counting two, and saturates at 5.
 * Two solid blows -- four -- end the bout ($28D8). */
#define HIT_POINTS_MAX 5

#endif
