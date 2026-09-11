/* verify_hit.c - exercise the ported hit test.
 *
 * The test itself is $415D, transcribed in hit_test.h. This drives it over the whole
 * space of fighter configurations and checks the invariants the ROM's own structure
 * implies, then replays it against RAM dumps taken while the real game was fighting.
 *
 * Build and run:  make verify-hit
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hit_test.h"

static int failures;

static void check(int cond, const char* what)
{
    printf("  %-54s %s\n", what, cond ? "ok" : "FAILED");
    if (!cond) failures++;
}

static void place(Fighter* f, int x, int facing, int shape, int hold)
{
    memset(f, 0, sizeof *f);
    f->x = x; f->facing = facing; f->shape = shape; f->hold = hold;
}

int main(void)
{
    /* hit_test.h pulls in the whole state machine; this file only drives $415D */
    (void)fgtDispatch; (void)fgtUpdate; (void)fgtStartMove; (void)fgtApplyVelDir;
    Fighter a, b;
    const Fighter* pair[2] = { &a, &b };
    long tried = 0, hits = 0, solid = 0, glancing = 0;
    int badStrike = 0, badPart = 0, badReach = 0, badDist = 0, badClass = 0, badMove = 0;
    int perStrike[HIT_STRIKES];
    memset(perStrike, 0, sizeof perStrike);

    printf("sweeping every fighter configuration\n");
    for (int sa = 0; sa < HIT_SHAPES; sa++)
    for (int sb = 0; sb < HIT_SHAPES; sb++)
    for (int fa = 0; fa < 2; fa++)
    for (int fb = 0; fb < 2; fb++)
    for (int xa = ARENA_LEFT; xa <= ARENA_RIGHT; xa += 3)
    for (int xb = ARENA_LEFT; xb <= ARENA_RIGHT; xb += 3) {
        place(&a, xa, fa, sa, 0);
        place(&b, xb, fb, sb, 0);
        HitResult h = hitTest(pair, 0);
        tried++;
        if (!h.hit) continue;
        hits++;
        if (h.hit == 2) solid++; else glancing++;

        /* $416C: only a shape in $400B can strike */
        int s = h.strike;
        if (s < 0 || s >= HIT_STRIKES) { badStrike++; continue; }
        perStrike[s]++;
        const Fighter* att = h.attacker ? &b : &a;
        const Fighter* def = h.attacker ? &a : &b;
        if (att->shape != HIT_STRIKE_SHAPE[s]) badStrike++;
        /* $4203: the part must be one this strike reaches */
        if (!(HIT_PART_BIT[h.part] & HIT_STRIKE_PARTS[s])) badReach++;
        if (h.part < 0 || h.part >= HIT_PARTS) badPart++;
        /* $4286/$4290: the distance is inside [0, far] */
        if (h.distance < 0 || h.distance > HIT_FAR[s]) badDist++;
        /* $419D: the defender's shape must have been hittable */
        if (HIT_SHAPE_CLASS[def->shape] >= HIT_ABSENT) badClass++;
        /* $42D8: the reaction is a real move id */
        if (h.reaction >= NUM_MOVE_IDS) badMove++;
    }
    printf("    %ld configurations, %ld land a blow (%.2f%%): %ld solid, %ld glancing\n",
           tried, hits, 100.0 * hits / tried, solid, glancing);
    printf("    blows per strike shape:");
    for (int s = 0; s < HIT_STRIKES; s++)
        if (perStrike[s]) printf(" shape %d:%d", HIT_STRIKE_SHAPE[s], perStrike[s]);
    printf("\n");
    check(hits > 0, "some configurations land a blow");
    check(badStrike == 0, "$416C: only a shape listed in $400B ever strikes");
    check(badPart == 0 && badReach == 0, "$4203: the part struck is one the strike reaches");
    check(badDist == 0, "$4286/$4290: the distance is within [0, far]");
    check(badClass == 0, "$419D: a shape with no target class is never hit");
    check(badMove == 0, "$42D8: the forced reaction is a real move id");

    printf("a held pose has spent its blow ($4185)\n");
    {
        int before = 0, after = 0;
        for (int s = 0; s < HIT_STRIKES - 1; s++)
        for (int xb = ARENA_LEFT; xb <= ARENA_RIGHT; xb++) {
            place(&a, 0x50, 0, HIT_STRIKE_SHAPE[s], 0);
            place(&b, xb, 1, 0, 0);
            if (hitTest(pair, 0).hit) before++;
            a.hold = HIT_HOLD_LIMIT;
            if (hitTest(pair, 0).hit) after++;
        }
        printf("    with the pose fresh: %d blows; held for %d ticks: %d\n",
               before, HIT_HOLD_LIMIT, after);
        check(before > 0, "a fresh strike pose lands blows");
        check(after == 0, "the same pose held lands none");
    }

    printf("neither fighter is favoured\n");
    {
        int diff = 0;
        for (int s = 0; s < HIT_STRIKES - 1; s++)
        for (int xa = ARENA_LEFT; xa <= ARENA_RIGHT; xa += 2)
        for (int xb = ARENA_LEFT; xb <= ARENA_RIGHT; xb += 2) {
            place(&a, xa, 0, HIT_STRIKE_SHAPE[s], 0);
            place(&b, xb, 1, 0, 0);
            HitResult h1 = hitTest(pair, 0);
            /* the same pair the other way round: $4165 only chooses who is tried first */
            place(&a, xa, 0, HIT_STRIKE_SHAPE[s], 0);
            place(&b, xb, 1, 0, 0);
            HitResult h2 = hitTest(pair, 1);
            if (h1.hit != h2.hit || h1.strike != h2.strike || h1.part != h2.part) diff++;
        }
        printf("    %d configurations differ by which fighter $6115 names first\n", diff);
        check(diff == 0, "the parity only picks who is examined first");
    }

    printf("against the machine: RAM dumps taken during a fight\n");
    {
        int n = 0, predicted = 0, dumpSaid = 0;
        for (int i = 0; i < 30; i++) {
            char path[128];
            snprintf(path, sizeof path, "extracted/fightdumps/dump_%02d.bin", i);
            FILE* fp = fopen(path, "rb");
            if (!fp) continue;
            static unsigned char d[0x10000];
            size_t got = fread(d, 1, sizeof d, fp);
            fclose(fp);
            if (got < 0x6200) continue;
            n++;
            place(&a, d[0xE0], d[0xE2] & 1, d[0xDD], d[0x6185]);
            place(&b, d[0xE1], d[0xE3] & 1, d[0xDE], d[0x6186]);
            if (hitTest(pair, d[0x6115] & 1).hit) predicted++;
            if (d[0xD8]) dumpSaid++;
        }
        if (!n) {
            printf("    no fight dumps present -- skipped\n");
        } else {
            printf("    %d dumps: the port reports a blow in %d, the game's $00D8 in %d\n",
                   n, predicted, dumpSaid);
            printf("    (the two are one tick apart -- $415D runs after $3BF9, and the\n"
                   "     dump catches the next tick's fighter state, so this bounds how\n"
                   "     often the routine fires, it does not pair states with outcomes)\n");
            check(predicted == 0 && dumpSaid == 0,
                  "neither the port nor the game reports a blow at those moments");
        }
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "all checks passed",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
