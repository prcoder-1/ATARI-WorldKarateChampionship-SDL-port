/* popup.h - the points a blow scores, thrown up on the ground.
 *
 * Not a banner and not the word BONUS: just the number, 100 to 1600, in
 * four characters. $46F0 swaps the digits into characters $30/$31 of the
 * lower character set by the announcement number $613F, $46DA supplies the
 * trailing "00" in $32/$33, and $4660 lays codes $B0..$B3 into row 4 of the
 * ground at the column $6140 the hit test computed.
 *
 * Bit 7 of those codes sends pixel value 3 to COLPF3, which is $0F for this
 * band; value 1 is COLPF0, $00. So: white digits, black shadow.
 * GENERATED FILE - do not edit; re-run extract_popup.py instead. */
#ifndef POPUP_H
#define POPUP_H
#include <stdint.h>
#define POPUP_COUNT 6
#define POPUP_CHARS 4
#define POPUP_LINES 8
#define POPUP_PX_PER_CHAR 4
#define POPUP_CLOCKS_PER_PX 2
/* row 4 of the ground text region, measured on a capture that caught one */
#define POPUP_TOP 161
#define POPUP_LEFT 32
/* the colours the band's COLPF3 and COLPF0 put on screen */
#define POPUP_COL_INK 211,211,211
#define POPUP_COL_SHADOW 4,4,4
/* mode 4 pixel values, 0 and 2 transparent here */
static const uint8_t POPUP_PX[POPUP_COUNT][POPUP_LINES][POPUP_CHARS*POPUP_PX_PER_CHAR]={
  {{0,0,0,0,0,0,3,0,3,3,3,0,3,3,3,0},{0,0,0,0,0,3,3,1,3,1,3,1,3,1,3,1},{0,0,0,0,0,1,3,1,3,1,3,1,3,1,3,1},{0,0,0,0,0,0,3,1,3,1,3,1,3,1,3,1},{0,0,0,0,0,0,3,1,3,3,3,1,3,3,3,1},{0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
  {{0,0,0,0,3,3,3,0,3,3,3,0,3,3,3,0},{0,0,0,0,0,1,3,1,3,1,3,1,3,1,3,1},{0,0,0,0,3,3,3,1,3,1,3,1,3,1,3,1},{0,0,0,0,3,1,1,1,3,1,3,1,3,1,3,1},{0,0,0,0,3,3,3,1,3,3,3,1,3,3,3,1},{0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
  {{0,0,0,0,3,0,3,0,3,3,3,0,3,3,3,0},{0,0,0,0,3,1,3,1,3,1,3,1,3,1,3,1},{0,0,0,0,3,3,3,1,3,1,3,1,3,1,3,1},{0,0,0,0,0,1,3,1,3,1,3,1,3,1,3,1},{0,0,0,0,0,0,3,1,3,3,3,1,3,3,3,1},{0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
  {{0,0,0,0,3,3,3,0,3,3,3,0,3,3,3,0},{0,0,0,0,3,1,1,1,3,1,3,1,3,1,3,1},{0,0,0,0,3,3,3,1,3,1,3,1,3,1,3,1},{0,0,0,0,0,1,3,1,3,1,3,1,3,1,3,1},{0,0,0,0,3,3,3,1,3,3,3,1,3,3,3,1},{0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
  {{0,0,0,0,3,3,3,0,3,3,3,0,3,3,3,0},{0,0,0,0,3,1,3,1,3,1,3,1,3,1,3,1},{0,0,0,0,3,3,3,1,3,1,3,1,3,1,3,1},{0,0,0,0,3,1,3,1,3,1,3,1,3,1,3,1},{0,0,0,0,3,3,3,1,3,3,3,1,3,3,3,1},{0,0,0,0,0,1,1,1,1,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
  {{0,0,3,0,3,3,3,0,3,3,3,0,3,3,3,0},{0,3,3,1,3,1,3,1,3,1,3,1,3,1,3,1},{0,1,3,1,3,1,3,1,3,1,3,1,3,1,3,1},{0,0,3,1,3,1,3,1,3,1,3,1,3,1,3,1},{0,0,3,1,3,3,3,1,3,3,3,1,3,3,3,1},{0,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1},{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0}},
};
#endif
