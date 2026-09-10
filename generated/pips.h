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
/* $450B clamps a fighter's points here */
#define PIP_POINTS_MAX 5
/* $4514: which dots light is a lookup, not a calculation -- $44A7 picks
 * an eleven-scanline pattern out of $44AE by the fighter's points. These
 * are those six patterns, reduced to one bit per dot. */
static const uint8_t PIP_LIT[6]={0x00,0x04,0x02,0x06,0x03,0x07};
/* and the patterns themselves, as the ROM holds them */
static const uint8_t PIP_PATTERN[6][11]={
  {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
  {0x00,0x00,0x00,0x00,0x00,0x00,0x06,0x0F,0x0F,0x0F,0x06},
  {0x06,0x0F,0x0F,0x0F,0x06,0x00,0x00,0x00,0x00,0x00,0x00},
  {0x06,0x0F,0x0F,0x0F,0x06,0x00,0x06,0x0F,0x0F,0x0F,0x06},
  {0x66,0xFF,0xFF,0xFF,0x66,0x00,0x00,0x00,0x00,0x00,0x00},
  {0x66,0xFF,0xFF,0xFF,0x66,0x00,0x06,0x0F,0x0F,0x0F,0x06},
};
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
