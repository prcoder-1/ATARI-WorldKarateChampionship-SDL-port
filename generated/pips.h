/* pips.h - the HUD's ippon markers, captured from the screen.
 *
 * Two Points on the upper line and one Half-Point below it, per player,
 * the lower dot under the right-hand of the pair. A dot is drawn dark or
 * lit; the upper pair fills right to left with full points and the lower
 * one lights on its own for a half point outstanding.
 *
 * They are Player/Missile objects over the HUD, not characters, so they are
 * measured off the screen like the referee and his signs.
 * GENERATED FILE - do not edit; re-run extract_pips.py instead. */
#ifndef PIPS_H
#define PIPS_H
#include <stdint.h>
#define PIP_W 8
#define PIP_H 5
#define PIP_COUNT 3
#define PIP_ORIGIN_P1 88
#define PIP_ORIGIN_P2 280
#define PIP_TOP 17
/* the lower dot is the half point; the first two are the full points */
#define PIP_HALF_INDEX 2
static const int8_t PIP_DX[PIP_COUNT]={0,8,8};
static const int8_t PIP_DY[PIP_COUNT]={0,0,6};
/* the colours the game puts on screen */
#define PIP_COL_DARK 104,27,35
#define PIP_COL_LIT 250,204,144
static const uint8_t PIP_PX[PIP_H*PIP_W]={0,0,1,1,1,1,0,0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,0,1,1,1,1,0,0};
#endif
