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
 * $5DE0 is the name entry, and it is here too. The loser picks three characters, one at
 * a time: the stick steps through them and the fire button takes each. $616C is the one
 * being shown, $23..$3E in the lower character set's codes -- space, A to Z, and the
 * dash the table starts with -- and it blinks off $14.
 *
 *   5E1C: LDA $616D / BNE $5E29      ; TRIG is 1 when released -> step
 *   5E21:   LDA $616E / BNE $5E7F    ; released last pass, pressed now -> take it
 *   5E2E: LDA $616B / CMP #$05 / BCC ; $616B is bumped every video frame ($3956)
 *   5E3A: AND #$04 -> DEC $616C      ; left, active low; under $23 wraps to $3E
 *   5E50: AND #$08 -> INC $616C      ; right; over $3E wraps to $23
 *   5E98: INC $6170 / CMP #$03       ; three characters and it is done
 *   5E0F: LDA $6171 / CMP #$0A       ; $6171 is bumped every 256 frames ($394E)
 *
 * What the port does not carry over is the walk: $5CFF puts the loser's fighter at the
 * left of the arena and $5D62/$5DA4 animate him. Here the keys do the picking.
 */
#ifndef HISCORE_TABLE_H
#define HISCORE_TABLE_H

#include <stdio.h>
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

/* $5DE0: the state of one name entry */
#define HS_CH_FIRST  (0x23 - 0x19)   /* space */
#define HS_CH_LAST   (0x3E - 0x19)   /* the dash the table starts with */
#define HS_CH_START  (0x24 - 0x19)   /* A */
#define HS_NAME_REPEAT  5            /* $5E2E, on $616B */
#define HS_NAME_TIMEOUT (10 * 256)   /* $5E0F, on $6171 */

typedef struct { int ch, pos, held, repeat, timer; uint8_t buf[HS_NAME_LEN]; } HsName;

static void hsNameBegin(HsName* n, const uint8_t* start)
{
    memcpy(n->buf, start, HS_NAME_LEN);
    n->ch = HS_CH_START;                     /* $5DF4 */
    n->pos = n->held = n->repeat = n->timer = 0;
}

/* One pass, once per video frame. `stick` is the Atari direction nibble, active low;
 * `fire` is true while the button is down. Returns 1 when the entry is finished. */
static int hsNameTick(HsName* n, int stick, int fire)
{
    if (++n->timer >= HS_NAME_TIMEOUT) return 1;      /* $5E0F */
    n->repeat++;                                       /* $3956 */

    if (!fire) {                                       /* $5E1C: TRIG released */
        n->held = 1;                                   /* $5E29 */
        if (n->repeat >= HS_NAME_REPEAT) {             /* $5E2E */
            n->repeat = 0;
            if (!(stick & 0x04)) {                     /* $5E3A: left */
                if (--n->ch < HS_CH_FIRST) n->ch = HS_CH_LAST;
            } else if (!(stick & 0x08)) {              /* $5E50: right */
                if (++n->ch > HS_CH_LAST) n->ch = HS_CH_FIRST;
            }
        }
    } else if (n->held) {                              /* $5E21: pressed on the edge */
        n->held = 0;                                   /* $5E7F */
        n->buf[n->pos] = (uint8_t)n->ch;
        n->ch = HS_CH_START;
        n->repeat = 0; n->timer = 0;
        if (++n->pos >= HS_NAME_LEN) return 1;         /* $5E98 */
    }
    return 0;
}

/* Keeping the table between runs is the PORT'S doing. The original has nowhere to put
 * it: the table lives in RAM at $6220..$624F and a power cycle takes it with it. The
 * file is plain text so it can be read, edited or thrown away by hand, and anything the
 * port does not recognise in it falls back to the table the game starts with. */
#define HS_FILE_MAGIC "worldkarate-hiscore 1"

static void hsSave(const char* path)
{
    FILE* f = fopen(path, "w");
    if (!f) return;                       /* not being able to save is not fatal */
    fprintf(f, "%s\n", HS_FILE_MAGIC);
    for (int i = 0; i < HS_ENTRIES; i++)
        fprintf(f, "%d %d %d %d %d %d\n", hsTable[i].hi, hsTable[i].lo, hsTable[i].belt,
                hsTable[i].name[0], hsTable[i].name[1], hsTable[i].name[2]);
    fclose(f);
}

static void hsLoad(const char* path)
{
    hsReset();                            /* whatever happens, the table is valid after */
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[128];
    if (!fgets(line, sizeof line, f) || strncmp(line, HS_FILE_MAGIC, strlen(HS_FILE_MAGIC))) {
        fclose(f);
        return;
    }
    HsRow got[HS_ENTRIES];
    for (int i = 0; i < HS_ENTRIES; i++) {
        int hi, lo, belt, a, b, c;
        if (fscanf(f, "%d %d %d %d %d %d", &hi, &lo, &belt, &a, &b, &c) != 6
            || (unsigned)hi > 0x99 || (unsigned)lo > 0x99 || (unsigned)belt > 5
            || (unsigned)a > 0x7F || (unsigned)b > 0x7F || (unsigned)c > 0x7F) {
            fclose(f);
            return;                       /* short or malformed: keep the fresh table */
        }
        got[i].hi = hi; got[i].lo = lo; got[i].belt = belt;
        got[i].name[0] = (uint8_t)a; got[i].name[1] = (uint8_t)b; got[i].name[2] = (uint8_t)c;
    }
    fclose(f);
    for (int i = 0; i < HS_ENTRIES; i++) hsTable[i] = got[i];
    hsSort();                             /* a hand-edited file need not be in order */
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
