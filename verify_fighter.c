/* verify_fighter.c - drive the ported fighter state machine headlessly.
 *
 * The point is to check that the machine recovered from the 6502 actually behaves:
 * every move reaches the end of its own frame range and returns to stand, walking
 * moves the fighter, attacks pass through their live frames, and nothing gets stuck
 * or runs off the arena.
 *
 * Build and run:  make verify-fighter
 */
#include <stdio.h>
#include <string.h>

#include "fighter.h"
#include "ai.h"
#include "generated/timing.h"
#include "generated/pips.h"
#include "hud.h"
#include "hiscore.h"

static unsigned rngstate = 0x1234;
static unsigned rnd(void) { rngstate = rngstate * 1103515245u + 12345u; return (rngstate >> 16) & 0x7fff; }

/* Atari stick nibble, active low */
#define ST_CENTRE 0x0F
#define ST_UP     (ST_CENTRE & ~0x01)
#define ST_DOWN   (ST_CENTRE & ~0x02)
#define ST_LEFT   (ST_CENTRE & ~0x04)
#define ST_RIGHT  (ST_CENTRE & ~0x08)

static int failures;

static void check(int cond, const char* what)
{
    printf("  %-52s %s\n", what, cond ? "ok" : "FAILED");
    if (!cond) failures++;
}

/* Hold one stick+fire for n frames, reporting what the machine did. */
static void run(Fighter* f, int stick, int fire, int frames,
                int* movesSeen, int* maxFrame, int* minX, int* maxX)
{
    for (int i = 0; i < frames; i++) {
        f->queued = fgtDispatch(f, stick, fire, 0);
        fgtUpdate(f, -1, rnd);
        if (movesSeen) movesSeen[f->move & 31]++;
        if (maxFrame && f->frame > *maxFrame) *maxFrame = f->frame;
        if (minX && f->x < *minX) *minX = f->x;
        if (maxX && f->x > *maxX) *maxX = f->x;
    }
}

/* $0050,y defaults to "joystick" here: most of $53BC is the joystick path, and the
 * CPU path gets its own section below. */
static void reset(Fighter* f, int x, int facing)
{
    memset(f, 0, sizeof *f);
    f->x = x; f->facing = facing;
    f->frame = f->next = MOVE_FRAME_START[0];
    f->shape = FRAME_SHAPE[f->frame];
    f->isHuman = 1;
}

int main(void)
{
    Fighter f;
    int seen[32], maxFrame, minX, maxX;

    printf("frame index stays inside each move's range\n");
    {
        int bad = 0;
        for (int m = 0; m < NUM_MOVE_IDS; m++) {
            reset(&f, 0x60, 0);
            f.queued = m;
            fgtStartMove(&f, m, rnd);
            for (int i = 0; i < 200; i++) {
                fgtUpdate(&f, -1, rnd);
                int lo = MOVE_FRAME_START[f.move], hi = MOVE_FRAME_START[f.move + 1];
                if (f.frame < 0 || f.frame >= NUM_FRAMES || f.frame < lo || f.frame >= hi) {
                    if (bad < 5)
                        printf("    started %2d: move %2d frame %3d outside [%d,%d)\n",
                               m, f.move, f.frame, lo, hi);
                    bad++;
                }
            }
        }
        check(bad == 0, "every move keeps frame within MOVE_FRAME_START bounds");
    }

    printf("moves terminate and fall back to stand\n");
    {
        int stuck = 0;
        for (int m = 1; m < NUM_MOVE_IDS; m++) {
            reset(&f, 0x60, 0);
            fgtStartMove(&f, m, rnd);
            int ended = 0;
            for (int i = 0; i < 300; i++) {
                f.queued = 0;                 /* release the stick */
                fgtUpdate(&f, -1, rnd);
                /* ATTR_LOCK ends a fighter deliberately: moves 17-19 are the struck
                   and knocked-down reactions, which stay down until the bout resets. */
                if (f.move == 0 || f.move == MOVE_IDLE || f.locked) { ended = 1; break; }
            }
            if (!ended) { stuck++; printf("    move %d never ended\n", m); }
        }
        check(stuck == 0, "every move ends: back to stand, or locked down");
    }

    printf("walking moves the fighter, and the arena bounds hold\n");
    {
        memset(seen, 0, sizeof seen); maxFrame = 0; minX = 999; maxX = -999;
        reset(&f, 0x60, 0);
        int x0 = f.x;
        run(&f, ST_RIGHT, 0, 240, seen, &maxFrame, &minX, &maxX);
        check(f.x != x0, "walking forward changes x");
        check(seen[AM_WALK_F] > 0, "walk-forward move is selected by stick right");
        check(minX >= ARENA_LEFT, "never crosses the left arena wall");

        reset(&f, 0x60, 0);
        minX = 999; maxX = -999;
        run(&f, ST_RIGHT, 0, 2000, NULL, NULL, &minX, &maxX);
        int w = SHAPE_WIDTH[f.shape % NUM_SHAPES];
        check(f.x + w <= ARENA_RIGHT + 1, "never crosses the right arena wall");
    }

    printf("attacks are selected, and held poses hold\n");
    {
        int attacks = 0;
        reset(&f, 0x60, 0);
        f.isHuman = 1;
        for (int i = 0; i < 600; i++) {
            f.queued = fgtDispatch(&f, ST_RIGHT, 1, 0);
            fgtUpdate(&f, -1, rnd);
            if (am_is_attack(f.move)) attacks++;
        }
        check(attacks > 0, "fire + forward selects an attack move");

        /* $6185: holding the stick on an ATTR_HOLD frame stalls the animation */
        reset(&f, 0x60, 0);
        f.isHuman = 1;
        int heldFrame = -1, heldFor = 0;
        for (int i = 0; i < 400; i++) {
            f.queued = fgtDispatch(&f, ST_DOWN, 0, 0);   /* crouch, a held pose */
            fgtUpdate(&f, -1, rnd);
            if (f.attr & ATTR_HOLD) {
                if (f.frame == heldFrame) heldFor++;
                heldFrame = f.frame;
            }
        }
        printf("    crouch held on one frame for %d ticks, $6185 = %d\n", heldFor, f.hold);
        check(heldFor > 10, "a held frame stalls the animation");
        check(f.hold > 0, "$6185 counts the hold");
    }

    /* $53D6 and $543F both test $0050,y and skip the whole block when it is zero, so a
     * CPU fighter plays every move it starts straight through: no reverse unwind, no
     * hold, no re-dispatch partway. The machine agrees -- across 30 RAM dumps taken
     * during a fight, all 60 samples of a CPU fighter have $6187 = $6185 = 0. */
    printf("a CPU-controlled fighter plays each move straight through\n");
    {
        int reversed = 0, held = 0, restarted = 0;
        for (int m = 1; m < NUM_MOVE_IDS; m++) {
            reset(&f, 0x60, 0);
            f.isHuman = 0;
            f.queued = m; fgtStartMove(&f, -1, rnd); f.queued = 0;
            int prevMove = -1, prevFrame = -1;
            for (int i = 0; i < 120; i++) {
                /* the stick is irrelevant to a CPU fighter, but keep it asking for
                   something else, which is exactly what used to break the animation */
                f.queued = (i & 1) ? AM_WALK_F : 0;
                fgtUpdate(&f, -1, rnd);
                if (f.reverse) reversed++;
                if (f.hold) held++;
                /* a move that runs off its own end and is picked again restarts
                   legitimately; a move that jumps back from the middle does not */
                if (f.move == prevMove && f.frame < prevFrame
                    && prevFrame + 1 < MOVE_FRAME_START[prevMove + 1]) {
                    if (restarted < 4)
                        printf("    move %2d: frame went %3d -> %3d, mid-range\n",
                               f.move, prevFrame, f.frame);
                    restarted++;
                }
                prevMove = f.move; prevFrame = f.frame;
                if (f.locked) break;
            }
        }
        check(reversed == 0, "$6187 stays clear: a CPU fighter never unwinds a pose");
        check(held == 0, "$6185 stays clear: a CPU fighter never holds a frame");
        check(restarted == 0, "and never restarts a move partway through it");
    }

    /* $530F's first branch: while $00D8 freezes play, the queued move is overridden by
     * $6131 rather than obeyed. Without it a move still queued when the blow landed
     * repeats for the whole freeze -- and nothing can stop it, because $274A skips the
     * joystick read, so releasing the keys is not even looked at. */
    printf("a move queued when play freezes does not repeat ($530F/$00D8)\n");
    {
        int repeats = 0, ticks = 24;
        reset(&f, 0x60, 0);
        f.queued = AM_ATK_U;                       /* the jump kick, fire + up */
        fgtStartMove(&f, -1, rnd);
        for (int i = 0; i < ticks; i++) {
            /* the queue is NOT refreshed: no key is read while play is frozen */
            fgtUpdate(&f, -1, rnd);                /* as the port used to do */
            /* the first tick replays the start frame; a later one means a restart */
            if (i && f.move == AM_ATK_U && f.frame == MOVE_FRAME_START[AM_ATK_U])
                repeats++;
        }
        printf("    obeying the stale queue: the move restarts %d times in %d ticks\n",
               repeats, ticks);
        check(repeats > 0, "the stale queue really does make it repeat");

        int again = 0;
        reset(&f, 0x60, 0);
        f.queued = AM_ATK_U;
        fgtStartMove(&f, -1, rnd);
        for (int i = 0; i < ticks; i++) {
            fgtUpdate(&f, 0, rnd);                 /* $6131 is 0 here ($289D/$2D9F) */
            if (i && f.move == AM_ATK_U && f.frame == MOVE_FRAME_START[AM_ATK_U])
                again++;
        }
        printf("    imposing $6131 instead: %d restarts, ending on move %d\n",
               again, f.move);
        check(again == 0, "the imposed move stops it restarting");
        check(f.move == 0, "and the fighter is left standing");
    }

    printf("horizontal velocity and the arena clamps ($5509/$553E/$5564)\n");
    {
        /* $550E branches on the direction flag: clear adds FRAME_VELX, set subtracts.
         * The reverse path at $54FF passes reverse EOR facing, so an unwinding pose
         * slides back the way it came -- which is the whole point of the flag. */
        int moved = 0, opposite = 0;
        for (int fr = 0; fr < NUM_FRAMES; fr++) {
            if (!FRAME_VELX[fr]) continue;
            moved++;
            reset(&f, 0x60, 0); f.frame = fr; f.shape = FRAME_SHAPE[fr];
            int base = f.x;
            fgtApplyVelDir(&f, 0);
            int fwd = f.x - base;
            reset(&f, 0x60, 0); f.frame = fr; f.shape = FRAME_SHAPE[fr];
            fgtApplyVelDir(&f, 1);
            int back = f.x - base;
            if (fwd == -back && fwd != 0) opposite++;
        }
        printf("    %d frames carry a velocity; %d reverse cleanly\n", moved, opposite);
        check(moved > 0, "some frames carry a horizontal velocity");
        check(opposite == moved, "the direction flag mirrors the step exactly");

        /* Where each of the four clamps parks the fighter, applied once to a position
         * already over the line. The extent comes from the direction flag and the
         * landing from the facing, which is how $5509 and $553E/$5564 split the work. */
        int fz = 0;                      /* a frame that carries no velocity */
        while (fz < NUM_FRAMES && FRAME_VELX[fz]) fz++;
        int w = SHAPE_WIDTH[0];

        reset(&f, 0x08, 0); f.frame = fz; f.shape = 0;
        fgtApplyVelDir(&f, 0);
        check(f.x == ARENA_LEFT, "$5549: facing right, the left wall parks x at $10");

        reset(&f, 0x02, 1); f.frame = fz; f.shape = 0;
        fgtApplyVelDir(&f, 1);
        check(f.x == 0x12 + w - 0x24, "$5551: facing left, it parks at $12 + width - $24");

        reset(&f, 0xA8, 0); f.frame = fz; f.shape = 0;
        fgtApplyVelDir(&f, 0);
        check(f.x == ARENA_RIGHT - w, "$556F: facing right, the right wall parks at $AE - width");

        reset(&f, 0x9C, 1); f.frame = fz; f.shape = 0;
        fgtApplyVelDir(&f, 1);
        check(f.x == 0x8A, "$557B: facing left, the right wall parks at $8A");
    }

    printf("every dispatch entry maps to a real move\n");
    {
        int bad = 0;
        for (int i = 0; i < 32; i++) {
            if (DISPATCH_R[i] >= NUM_MOVE_IDS) bad++;
            if (DISPATCH_L[i] >= NUM_MOVE_IDS) bad++;
            if (DISPATCH_altR[i] >= NUM_MOVE_IDS) bad++;
            if (DISPATCH_altL[i] >= NUM_MOVE_IDS) bad++;
        }
        check(bad == 0, "all four dispatch tables stay within the move set");
    }

    printf("every animation frame maps to a real shape\n");
    {
        int bad = 0, hi = 0;
        for (int i = 0; i < NUM_FRAMES; i++) {
            if (FRAME_SHAPE[i] > hi) hi = FRAME_SHAPE[i];
            if (FRAME_SHAPE[i] >= NUM_SHAPES) bad++;
        }
        printf("    %d moves, %d frames, highest shape id %d of %d\n",
               NUM_MOVE_IDS, NUM_FRAMES, hi, NUM_SHAPES);
        check(bad == 0, "FRAME_SHAPE never leaves the shape table");
    }

    printf("fighter-to-fighter geometry ($3F92)\n");
    {
        Fighter a, b;
        reset(&a, 0x30, 0);
        reset(&b, 0x80, 1);
        FgtGeom g = fgtGeometry(&a, &b);
        check(g.gap > 0, "a gap is measured between separated fighters");
        check(g.quarter == g.gap >> 2, "$6136 is the gap in quarters");
        check(g.code >= 0 && g.code < 8, "the situation code fits MOVE_GATE");
        reset(&b, 0x30, 1);
        FgtGeom touching = fgtGeometry(&a, &b);
        check(touching.gap < g.gap, "closing the distance shrinks the gap");
    }

    printf("the CPU opponent ($3D04)\n");
    {
        Fighter a, b;
        int chosen[64];
        memset(chosen, 0, sizeof chosen);
        int decided = 0, bad = 0;
        /* sweep every distance, both orientations and all five skill levels */
        for (int skill = 0; skill <= 5; skill++) {
            for (int gx = ARENA_LEFT; gx < ARENA_RIGHT - 24; gx += 3) {
                for (int fc = 0; fc < 2; fc++) {
                    for (int om = 0; om < NUM_MOVE_IDS; om++) {
                        reset(&a, gx, fc);
                        reset(&b, gx + 24, fc ^ 1);
                        a.isHuman = 0; b.isHuman = 1;
                        b.move = om;
                        b.frame = MOVE_FRAME_START[om];
                        int m = aiChooseMove(&a, &b, skill, rnd);
                        if (m < 0) continue;
                        decided++;
                        if (m < 0 || m >= NUM_MOVE_IDS) { bad++; if (bad < 4)
                            printf("    skill %d gap %d move %d -> %d out of range\n",
                                   skill, gx, om, m); }
                        else chosen[m]++;
                    }
                }
            }
        }
        printf("    %d decisions taken\n", decided);
        check(decided > 0, "the routine reaches a decision");
        check(bad == 0, "every chosen move is a real move id");
        int distinct = 0;
        for (int i = 0; i < NUM_MOVE_IDS; i++) if (chosen[i]) distinct++;
        int attacksChosen = 0;
        for (int i = 0; i < NUM_MOVE_IDS; i++)
            if (chosen[i] && am_is_attack(i)) attacksChosen++;
        printf("    %d distinct moves chosen, %d of them attacks\n",
               distinct, attacksChosen);
        check(attacksChosen > 0, "the CPU does attack");
        check(distinct >= 8, "the tables produce a varied move set");
    }

    /* $3D96: the whole defensive branch is behind a RANDOM gate whose threshold is
     * AI_RND_CLOSE[skill]. There is no block pose in this game -- defence is evasion:
     * $3DC4's AI_REPLY0 crouches under a high attack or steps back (5/6/27/30), and
     * $3DB9's AI_REPLY1 jumps over a low one (1/16/27/12). The port used to reset the
     * skill from 5 back to 1, which dropped the CPU into its most passive setting. */
    printf("the CPU's defensive branch opens with skill ($3D96)\n");
    {
        int replied[6], decided[6];
        memset(replied, 0, sizeof replied); memset(decided, 0, sizeof decided);
        int defensive = 0, monotone = 1;
        Fighter a, b;
        for (int skill = 0; skill <= 5; skill++) {
            for (int trial = 0; trial < 4000; trial++) {
                reset(&a, 0x50, 0); reset(&b, 0x60, 1);
                a.isHuman = 0; b.isHuman = 1;
                /* an opponent mid-attack, close enough for $3D46 to let it through */
                b.move = AM_ATK_F;
                b.frame = MOVE_FRAME_START[AM_ATK_F];
                int m = aiChooseMove(&a, &b, skill, rnd);
                if (m < 0) continue;
                decided[skill]++;
                for (int i = 0; i < 8; i++)
                    if (m == AI_REPLY0[i] || m == AI_REPLY1[i]) { replied[skill]++; break; }
            }
        }
        printf("    skill: gate -> acts at all / of those, a defensive reply\n");
        for (int skill = 0; skill <= 5; skill++) {
            printf("      %d: AI_RND_CLOSE %3d -> acts %2d%% of the time, replies %2d%%"
                   " of all trials\n",
                   skill, AI_RND_CLOSE[skill], 100 * decided[skill] / 4000,
                   100 * replied[skill] / 4000);
            if (replied[skill]) defensive++;
        }
        for (int skill = 1; skill <= 5; skill++)
            if (replied[skill] + 40 < replied[skill-1]) monotone = 0;
        check(defensive > 0, "the CPU does choose a defensive reply");
        check(monotone, "and does so more often as the skill rises");
        check(AI_RND_CLOSE[5] < AI_RND_CLOSE[1],
              "$3E4E: the gate opens wider at higher skill");
    }

    /* $2BA2 ends an ordinary bout on the clock or on four points, and on nothing else.
     * $3988 takes one BCD second off $00DC every $3C frames. The port used to end the
     * round after eight of the referee's traversals instead -- 13.4 s. */
    printf("the round runs its full clock ($3988/$2BA2)\n");
    {
        for (int two = 0; two < 2; two++) {
            int clk = two ? T_CLOCK_2P : T_CLOCK_1P, tick = T_CLOCK_TICK, frames = 0;
            while (clk && frames < 60 * 200) {
                frames++;
                if (--tick <= 0) {
                    tick = T_CLOCK_TICK;
                    int lo = clk & 0x0F, hi = clk >> 4;
                    if (lo) lo--; else { lo = 9; if (hi) hi--; }
                    clk = (hi << 4) | lo;
                }
            }
            int want = two ? 60 : 30;
            printf("    %s: $%02X counts out in %d frames = %d s\n",
                   two ? "two players" : "one player ", two ? T_CLOCK_2P : T_CLOCK_1P,
                   frames, frames / 60);
            check(frames / 60 == want, two ? "a two-player round lasts 60 seconds"
                                           : "a one-player round lasts 30 seconds");
        }
    }

    /* $4514: which ippon dots light is a lookup in $44AE by the fighter's points, not a
     * calculation. extract_pips.py reduces those patterns to PIP_LIT; this checks the
     * reduction against the patterns the header also carries. */
    printf("the ippon markers follow the ROM's own table ($44A7/$44AE)\n");
    {
        int bad = 0;
        for (int pts = 0; pts <= PIP_POINTS_MAX; pts++)
            for (int i = 0; i < PIP_COUNT; i++) {
                int on = 0;
                for (int y = 0; y < PIP_H; y++)
                    for (int x = 0; x < PIP_W; x++)
                        if (PIP_PX[y * PIP_W + x]
                            && (PIP_PATTERN[pts][PIP_DY[i] + y]
                                >> (7 - (PIP_DX[i] + x) / 2)) & 1) on = 1;
                if (on != ((PIP_LIT[pts] >> i) & 1)) bad++;
            }
        for (int pts = 0; pts <= PIP_POINTS_MAX; pts++) {
            char what[48]; what[0] = 0;
            if (PIP_LIT[pts] & 1) strcat(what, "upper left ");
            if (PIP_LIT[pts] & 2) strcat(what, "upper right ");
            if (PIP_LIT[pts] & 4) strcat(what, "lower");
            printf("    %d points: %s\n", pts, what[0] ? what : "none");
        }
        check(bad == 0, "PIP_LIT agrees with the $44AE patterns for every score");
        check(PIP_LIT[0] == 0, "no points lights nothing");
    }

    /* $5F66: the belt is read off the score, not awarded for winning. Checked against
     * the machine as well -- poking a score through the monitor and reading back $616A
     * agreed on both sides of all five thresholds, ten cases out of ten. */
    printf("the belt follows the score ($5F66/$5F95)\n");
    {
        static const int probe[10] = { 5900, 6000, 11900, 12000, 17900,
                                       18000, 25900, 26000, 39900, 40000 };
        static const int want[10]  = { 0, 1, 1, 2, 2, 3, 3, 4, 4, 5 };
        int bad = 0;
        for (int i = 0; i < 10; i++) {
            int got = hudBelt(probe[i]);
            if (got != want[i]) {
                bad++;
                printf("    %6d: belt %d, the machine said %d\n", probe[i], got, want[i]);
            }
        }
        printf("    0 -> %d, 5900 -> %d, 6000 -> %d, 26000 -> %d, 40000 -> %d,"
               " 999900 -> %d\n",
               hudBelt(0), hudBelt(5900), hudBelt(6000), hudBelt(26000),
               hudBelt(40000), hudBelt(999900));
        check(bad == 0, "every threshold falls where the machine puts it");
        check(hudBelt(0) == 0, "no score is a white belt");
        check(hudBelt(999900) == HUD_BELTS - 1, "$5F82's clamp keeps a huge score black");
    }

    /* $60B1/$60D3: the table keeps six entries and a candidate in slot 6, and sorts all
     * seven by score -- high byte then low. A score that cannot beat the last entry is
     * not taken ($5CDE's BCC). */
    printf("the high-score table sorts and cuts off as $60B1 does\n");
    {
        static const uint8_t nm[HS_NAME_LEN] = { 0, 0, 0 };
        hsReset();
        check(hsSubmit(0, 0, nm) < 0, "an empty score never places");
        int p1 = hsSubmit(5000, 0, nm);
        int p2 = hsSubmit(40000, 5, nm);
        int p3 = hsSubmit(12000, 2, nm);
        printf("    5000 -> %d, 40000 -> %d, 12000 -> %d (0 is the top row)\n",
               p1, p2, p3);
        check(p1 == 0, "the first real score goes to the top");
        check(p2 == 0, "a bigger one displaces it");
        check(p3 == 1, "and one in between lands in the middle");
        int ordered = 1;
        for (int i = 1; i < HS_ENTRIES; i++)
            if (hsScoreOf(&hsTable[i - 1]) < hsScoreOf(&hsTable[i])) ordered = 0;
        printf("    the table now reads:");
        for (int i = 0; i < HS_ENTRIES; i++) printf(" %d", hsScoreOf(&hsTable[i]));
        printf("\n");
        check(ordered, "the table is in descending order");
        /* fill it, then check the cut-off */
        for (int i = 0; i < HS_ENTRIES; i++) hsSubmit(50000 + i * 100, 5, nm);
        check(hsSubmit(100, 0, nm) < 0, "a score below the last entry is turned away");
        check(hsSubmit(90000, 5, nm) == 0, "and one above it is taken");
    }

    /* $5DE0: the loser picks three characters, the stick stepping and the button
     * taking. The range is $23..$3E in the lower character set's codes -- space, A to Z,
     * and the dash -- and it wraps at both ends ($5E4B/$5E61). */
    printf("the name entry steps, wraps and takes ($5DE0)\n");
    {
        HsName n;
        static const uint8_t dashes[HS_NAME_LEN] = { HS_CH_LAST, HS_CH_LAST, HS_CH_LAST };
        const int LEFT = 0x0F & ~0x04, RIGHT = 0x0F & ~0x08, CENTRE = 0x0F;

        hsNameBegin(&n, dashes);
        check(n.ch == HS_CH_START, "it starts on A");

        /* hold left long enough to step once, and check it wraps below space */
        int steps = 0;
        hsNameBegin(&n, dashes);
        while (n.ch != HS_CH_FIRST && steps < 200) { hsNameTick(&n, LEFT, 0); steps++; }
        printf("    left from A reaches space in %d frames\n", steps);
        check(n.ch == HS_CH_FIRST, "left walks down to space");
        for (int i = 0; i < HS_NAME_REPEAT; i++) hsNameTick(&n, LEFT, 0);
        check(n.ch == HS_CH_LAST, "and one more wraps to the dash");
        for (int i = 0; i < HS_NAME_REPEAT; i++) hsNameTick(&n, RIGHT, 0);
        check(n.ch == HS_CH_FIRST, "right wraps back the other way");

        /* three characters, each taken on the button's edge */
        hsNameBegin(&n, dashes);
        int done = 0;
        for (int c = 0; c < HS_NAME_LEN; c++) {
            for (int i = 0; i < HS_NAME_REPEAT * (c + 1); i++)
                if (hsNameTick(&n, RIGHT, 0)) done = 1;      /* pick */
            if (hsNameTick(&n, CENTRE, 1)) done = 1;         /* take */
        }
        printf("    entered %d %d %d, finished %s\n",
               n.buf[0], n.buf[1], n.buf[2], done ? "yes" : "no");
        check(done, "three characters ends the entry");
        check(n.buf[0] == HS_CH_START + 1 && n.buf[1] == HS_CH_START + 2
              && n.buf[2] == HS_CH_START + 3, "each one is the letter that was showing");

        /* holding the button does not take a second one: $5E21 needs the edge */
        hsNameBegin(&n, dashes);
        hsNameTick(&n, CENTRE, 0);
        hsNameTick(&n, CENTRE, 1);
        int was = n.pos;
        for (int i = 0; i < 20; i++) hsNameTick(&n, CENTRE, 1);
        check(n.pos == was, "$616E: holding the button takes only one");

        /* and it gives up on its own */
        hsNameBegin(&n, dashes);
        int frames = 0;
        while (!hsNameTick(&n, CENTRE, 0) && frames < HS_NAME_TIMEOUT * 2) frames++;
        printf("    left alone it gives up after %d frames\n", frames + 1);
        check(frames + 1 == HS_NAME_TIMEOUT, "$6171 ends it after ten of its ticks");
    }

    printf("$3F71 stays inside its limit\n");
    {
        int bad = 0, lo = 99, hi = -1;
        aiRandomFn = rnd;
        for (int lim = 1; lim <= 12; lim++)
            for (int i = 0; i < 500; i++) {
                int v = aiRandLimit(lim);
                if (v < 0 || v >= lim) bad++;
                if (lim == 12) { if (v < lo) lo = v; if (v > hi) hi = v; }
            }
        printf("    limit 12 produced values %d..%d\n", lo, hi);
        check(bad == 0, "aiRandLimit(n) always returns [0,n)");
        check(lo == 0 && hi == 11, "and covers the whole range");
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "all checks passed",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
