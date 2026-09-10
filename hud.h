/* hud.h - the game's HUD, drawn the way ANTIC drew it.
 *
 * Two rows of 40 ANTIC mode 4 characters at scanline 13, from the game's own character
 * set, with the colours recovered from a capture (extract_hud.py reproduces the capture
 * exactly). A mode 4 character is four pixels of two bits; the pixel value selects
 * COLBK, COLPF0, COLPF1, and then COLPF2 or COLPF3 depending on bit 7 of the character
 * code -- which is how the original gets several text colours out of one playfield.
 *
 * The layout is the original's, taken from its HUD routine at $5BB2..$5C8B:
 *
 *   col 10  "1UP", blank if player 1 is not human           ($5BA4/$5BA6/$5BA8)
 *   col 27  "2UP", the same in the COLPF3 colour
 *   col 14  "TIME" while anyone is playing, else "DEMO"     ($5BAA / $5BAE)
 *   col 19  two big digits: the round timer, BCD, leading zero blanked  ($5C71)
 *   col 23  one player: "L" and the level in COLPF3 digits  ($5C2A)
 *           two players: three markers, filled from each end by wins    ($5BFF)
 *   row 1, col 14   the belt name, 12 characters            ($5FB8)
 *   score: right-aligned, ending at col 5 for player 1 and col 39 for player 2
 *
 * The ippon markers over columns 7-8 and 31-32 are Player/Missile objects on the real
 * machine, not playfield; they are drawn here from the same bitmap ($324C).
 */
#ifndef HUD_H
#define HUD_H

#include <stdint.h>
#include <string.h>
#include "generated/hud.h"
#include "generated/pips.h"

#define HUD_CELLS (HUD_COLS * HUD_ROWS)

/* $5F66: which belt a score has earned.
 *
 * Not a rank handed out for winning: the game reads it straight off the score. $5F77
 * builds a byte out of the score's second and third digits -- ((F7 & $0F) << 4) |
 * (F8 >> 4), with the leading digit clamped to 9 if the score has run past 99999 --
 * and $5F95 walks HUD_BELT_THRESHOLD for the first entry it is under. Since the two
 * lowest digits of the score are always 00 ($455E only ever adds to the middle byte),
 * the thresholds work out as: WHITE below 6000, then YELLOW, GREEN, PURPLE, BROWN at
 * 6000, 12000, 18000 and 26000, and BLACK from 40000.
 *
 * Confirmed against the machine: poking a score through the monitor and reading back
 * $616A gives this belt on both sides of all five thresholds, ten cases out of ten.
 */
static int hudBelt(int score)
{
    int d1 = (score / 100000) % 10;
    int d2 = (score / 10000) % 10;
    int d3 = (score / 1000) % 10;
    int v = ((d1 ? 9 : d2) << 4) | d3;          /* $5F8B */
    for (int i = 0; i < HUD_BELTS; i++)
        if (v < HUD_BELT_THRESHOLD[i]) return i;
    return HUD_BELTS - 1;
}

/* the game's character codes: digits from $00, letters from $0B, space $0A */
static int hudDigit(int n) { return HUD_CH_DIGIT0 + (n % 10); }
static int hudBigDigit(int n) { return HUD_CH_BIGDIGIT0 + (n % 10); }

static void hudClear(uint8_t* s)
{
    memset(s, HUD_CH_SPACE, HUD_CELLS);
}

/* right-aligned decimal, as the score fields are */
static void hudNumberRight(uint8_t* s, int row, int endCol, int value, int digits, int alt)
{
    for (int i = 0; i < digits; i++) {
        int col = endCol - i;
        if (col < 0 || col >= HUD_COLS) continue;
        s[row * HUD_COLS + col] =
            (uint8_t)(hudDigit(value % 10) | (alt ? HUD_CH_COLOUR_ALT : 0));
        value /= 10;
        if (!value) break;                 /* no leading zeros */
    }
}

/* $5BB2: build the two rows. p1/p2Human mirror $0050/$0051, timerBcd is $00DC,
 * level is $00D3, wins are the two-player markers. The belt is not passed in: it is a
 * function of the score, see hudBelt(). */
static void hudCompose(uint8_t* s, int p1Human, int p2Human, int demo,
                       int timerBcd, int level, int p1Score, int p2Score,
                       int p1Wins, int p2Wins, int blankTimer)
{
    hudClear(s);

    if (p1Human) {                                   /* $5BCA */
        s[HUD_P1_LABEL_COL + 0] = (uint8_t)hudDigit(1);
        s[HUD_P1_LABEL_COL + 1] = HUD_CH_U;
        s[HUD_P1_LABEL_COL + 2] = HUD_CH_P;
    }
    if (p2Human) {
        s[HUD_P2_LABEL_COL + 0] = (uint8_t)(hudDigit(2) | HUD_CH_COLOUR_ALT);
        s[HUD_P2_LABEL_COL + 1] = HUD_CH_U | HUD_CH_COLOUR_ALT;
        s[HUD_P2_LABEL_COL + 2] = HUD_CH_P | HUD_CH_COLOUR_ALT;
    }

    /* $5BEE / $5C4E: "TIME" or "DEMO", in the charset's own coloured copies */
    {
        static const uint8_t timeLbl[4] = { 0x37, 0x38, 0x2A, 0x29 };   /* $5BAA */
        static const uint8_t demoLbl[4] = { 0x28, 0x29, 0x2A, 0x2B };   /* $5BAE */
        const uint8_t* lbl = demo ? demoLbl : timeLbl;
        for (int i = 0; i < 4; i++) s[HUD_LABEL_COL + i] = lbl[i];
    }

    /* $5C71: the round timer, two big digits, leading zero blanked */
    if (!blankTimer) {
        int hi = (timerBcd >> 4) & 0x0F, lo = timerBcd & 0x0F;
        s[HUD_TIMER_COL + 1] = (uint8_t)hudBigDigit(lo);
        s[HUD_TIMER_COL + 0] = hi ? (uint8_t)hudBigDigit(hi) : HUD_CH_SPACE;
    }

    if (p1Human && p2Human) {                        /* $5BFF: the win markers */
        for (int i = 0; i < 3; i++) s[HUD_RIGHT_COL + i] = HUD_CH_WIN_EMPTY;
        for (int i = 0; i < p1Wins && i < 3; i++) s[HUD_RIGHT_COL + i] = HUD_CH_WIN_P1;
        for (int i = 0; i < p2Wins && i < 3; i++) s[HUD_RIGHT_COL + 2 - i] = HUD_CH_WIN_P2;
    } else if (p1Human || p2Human) {                 /* $5C2A: "L" and the level */
        s[HUD_RIGHT_COL] = HUD_CH_L;
        s[HUD_RIGHT_COL + 1] = (uint8_t)(hudDigit((level >> 4) & 0x0F) | HUD_CH_COLOUR_ALT);
        s[HUD_RIGHT_COL + 2] = (uint8_t)(hudDigit(level & 0x0F) | HUD_CH_COLOUR_ALT);
    }

    /* $4570: both score fields are six digits and both are drawn, whether or not the
     * second fighter is a person -- $45A1/$45B2 blank the leading zeros of each. The
     * port used to draw the second only for a human, so in a one-player game the
     * computer's score never appeared. */
    hudNumberRight(s, 0, HUD_P1_SCORE_END, p1Score, 6, 0);
    hudNumberRight(s, 0, HUD_P2_SCORE_END, p2Score, 6, 1);

    /* $5F66/$5FB8: the belt name, and only outside a two-player game */
    if (!(p1Human && p2Human)) {
        /* $5F71: fighter 0's score if it is the person's, otherwise fighter 1's */
        const uint8_t* b = HUD_BELT[hudBelt(p1Human ? p1Score : p2Score)];
        for (int i = 0; i < HUD_BELT_LEN; i++)
            s[HUD_COLS + HUD_BELT_COL + i] = b[i];
    }
}

/* Render the two rows. setpx(x, y, r, g, b) is supplied by the caller. */
static void hudDraw(const uint8_t* s, int screenW, int screenH,
                    void (*setpx)(int, int, int, int, int))
{
    for (int row = 0; row < HUD_ROWS; row++) {
        for (int line = 0; line < HUD_ROW_LINES; line++) {
            int y = HUD_TOP + row * HUD_ROW_LINES + line;
            if (y < 0 || y >= screenH) continue;
            const uint8_t* lc = HUD_LINE_COL[row * HUD_ROW_LINES + line];
            for (int col = 0; col < HUD_COLS; col++) {
                int c = s[row * HUD_COLS + col];
                int glyph = HUD_FONT[c & 0x7F][line];
                int alt = (c & 0x80) ? 1 : 0;
                for (int k = 0; k < 4; k++) {
                    int v = (glyph >> (6 - 2 * k)) & 3;
                    HudCol p = HUD_PAL[lc[v == 3 ? (alt ? 4 : 3) : v]];
                    int x = HUD_LEFT + (col * 4 + k) * HUD_CLOCKS_PER_PX;
                    if (x + 1 < screenW) {
                        setpx(x, y, p.r, p.g, p.b);
                        setpx(x + 1, y, p.r, p.g, p.b);
                    }
                }
            }
        }
    }
}

/* $31E1/$4514: the ippon markers -- Players drawn over the HUD, three per fighter.
 *
 * Not a row of two, which is what this used to draw: **two Points on the upper line and
 * one Half-Point below**, the lower dot under the right-hand of the pair.
 *
 * Which of them light is not calculated. $4514 takes the fighter's points ($00D9,y,
 * clamped to 5 at $450B), indexes $44A7 to find an eleven-scanline pattern in $44AE and
 * copies it into the player strip. extract_pips.py reads those six patterns out of a
 * dump and reduces each to one bit per dot, which is PIP_LIT. The port used to derive
 * the lighting from points/2 and points&1 -- that agrees with the table for all six
 * values, but it was a guess and the game has the answer.
 *
 * $3892: when BOTH fighters are joystick-controlled the markers are parked at HPOS 0,
 * i.e. hidden, and the HUD shows the win markers instead ($5BFF). They only appear in a
 * one-player game or the demo.
 *
 * The dot, its three positions, both players' origins and the two colours are measured
 * off the screen by extract_pips.py, which re-renders them over 897 captures and refuses
 * to emit unless every pixel matches.
 */
static void hudMarkers(int p1Points, int p2Points, int p1Human, int p2Human,
                       int screenW, int screenH,
                       void (*setpx)(int, int, int, int, int))
{
    static const int origin[2] = { PIP_ORIGIN_P1, PIP_ORIGIN_P2 };
    static const uint8_t colour[2][3] = { { PIP_COL_DARK }, { PIP_COL_LIT } };
    const int pts[2] = { p1Points, p2Points };

    if (p1Human && p2Human) return;                  /* $3892 */

    for (int s = 0; s < 2; s++) {
        int n = pts[s];
        if (n < 0) n = 0;
        if (n > PIP_POINTS_MAX) n = PIP_POINTS_MAX;  /* $450B */
        for (int i = 0; i < PIP_COUNT; i++) {
            const uint8_t* c = colour[(PIP_LIT[n] >> i) & 1];
            for (int y = 0; y < PIP_H; y++) {
                int sy = PIP_TOP + PIP_DY[i] + y;
                if (sy < 0 || sy >= screenH) continue;
                for (int x = 0; x < PIP_W; x++) {
                    if (!PIP_PX[y * PIP_W + x]) continue;
                    int sx = origin[s] + PIP_DX[i] + x;
                    if (sx >= 0 && sx < screenW) setpx(sx, sy, c[0], c[1], c[2]);
                }
            }
        }
    }
}

#endif
