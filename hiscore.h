/* hiscore.h - the high-score table, kept and sorted the way the ROM keeps it.
 *
 * The game holds seven parallel arrays of seven slots -- six entries and, in slot 6, the
 * score just finished -- and bubble-sorts all seven, so the new one simply falls into
 * place. $60D3 compares the score's high byte and then its low; $60E4 swaps whole
 * entries. That is what is reproduced here, on a struct instead of seven arrays.
 *
 *   60B1: LDA #$06 / STA $61
 *   60B5:   $D6 = 0
 *   60B9:     X = $D6, Y = $D6 + 1, JSR $60D3, BCS -> skip, JSR $60E4 (swap)
 *   60C6:     INC $D6, CMP $61, BCC $60B9
 *   60CE:   DEC $61, BNE $60B5
 *
 * The score is BCD in two bytes with the two low digits always 00 ($455E only adds to
 * the middle byte), which is why the comparison needs no third byte.
 *
 * Not implemented, and it is the interesting half: the original lets the loser enter a
 * name by walking his fighter along a row of letters on the ground ($5CFF..$5D3C). The
 * port fills the name in and says so.
 */
#ifndef HISCORE_TABLE_H
#define HISCORE_TABLE_H

#include <string.h>
#include "generated/hiscore.h"

typedef struct { int hi, lo, belt; uint8_t name[HS_NAME_LEN]; } HsRow;

/* slot HS_ENTRIES is the candidate, exactly as $6226/$622D/$6234/$623B are */
static HsRow hsTable[HS_ENTRIES + 1];

static void hsReset(void)
{
    for (int i = 0; i < HS_ENTRIES; i++) {
        hsTable[i].hi = HS_START[i].hi;
        hsTable[i].lo = HS_START[i].lo;
        hsTable[i].belt = HS_START[i].belt;
        memcpy(hsTable[i].name, HS_START[i].name, HS_NAME_LEN);
    }
    memset(&hsTable[HS_ENTRIES], 0, sizeof hsTable[HS_ENTRIES]);
}

/* $60D3: is entry a worth less than entry b? (carry clear on return) */
static int hsLess(const HsRow* a, const HsRow* b)
{
    if (a->hi != b->hi) return a->hi < b->hi;
    return a->lo < b->lo;
}

/* $60B1: the bubble sort, over all seven slots */
static void hsSort(void)
{
    for (int n = HS_ENTRIES; n > 0; n--)
        for (int i = 0; i < n; i++)
            if (hsLess(&hsTable[i], &hsTable[i + 1])) {
                HsRow t = hsTable[i];
                hsTable[i] = hsTable[i + 1];
                hsTable[i + 1] = t;
            }
}

/* $5CC7..$5CEA: put the finished score in the candidate slot and sort it in. `score` is
 * the port's plain decimal; the ROM's two BCD bytes are its digits 1-2 and 3-4, the last
 * two digits always being 00. Returns the position it took, or -1 if it did not place. */
static int hsSubmit(int score, int belt, const uint8_t* name)
{
    int d1 = (score / 100000) % 10, d2 = (score / 10000) % 10;
    int d3 = (score / 1000) % 10,   d4 = (score / 100) % 10;
    HsRow* c = &hsTable[HS_ENTRIES];
    c->hi = (d1 << 4) | d2;
    c->lo = (d3 << 4) | d4;
    c->belt = belt;
    memcpy(c->name, name, HS_NAME_LEN);

    /* $5CDE: it only counts if it beats the last entry */
    if (!hsLess(&hsTable[HS_ENTRIES - 1], c)) return -1;
    /* $5CB2 clears the "this is the new one" markers and $5CC7 sets the candidate's, so
     * that $5CED can find where it ended up. Keep a copy instead: the sort moves the
     * candidate out of its slot, so comparing against that slot afterwards finds
     * whatever was swapped in. */
    HsRow want = *c;
    hsSort();
    for (int i = 0; i < HS_ENTRIES; i++)
        if (hsTable[i].hi == want.hi && hsTable[i].lo == want.lo
            && !memcmp(hsTable[i].name, want.name, HS_NAME_LEN)) return i;
    return -1;
}

static int hsScoreOf(const HsRow* r)
{
    return ((r->hi >> 4) * 100000 + (r->hi & 0x0F) * 10000
            + (r->lo >> 4) * 1000 + (r->lo & 0x0F) * 100);
}

/* $6035: an entry with no score at all has no belt either -- $603D passes $FF, and
 * $5ED8's CMP #$06 / BCS makes $5ED6 draw nothing. */
static int hsHasBelt(const HsRow* r) { return r->hi || r->lo; }

#endif
