/* referee.h - the referee, captured from the screen.
 *
 * A third figure on the same Player/Missile hardware as the fighters, in his
 * own scanline band. He STANDS STILL and signals from the spot: $6159 is how
 * far through one of his three actions he is, not where he is, and reaching
 * the end of that counter is what takes one off the round counter $6154
 * ($5807/$58EF/$5834).
 * GENERATED FILE - do not edit; re-run extract_referee.py instead. */
#ifndef REFEREE_H_DATA
#define REFEREE_H_DATA
#include <stdint.h>
/* He does not move: this is where he stands, in colour clocks and
 * scanlines, measured from the captures. */
#define REF_W 9
#define REF_H 32
#define REF_Y 137
#define REF_X 320
#define REF_CLOCKS_PER_PX 2
/* colour indices: 1 = gi, 2 = skin, 3 = outline */
static const uint8_t REF_PX[REF_H*REF_W]={
  0,0,0,2,2,2,0,0,0,
  0,0,2,2,2,2,2,0,0,
  0,0,2,3,3,3,2,0,0,
  0,0,2,3,2,3,2,0,0,
  0,0,2,2,2,2,2,0,0,
  0,0,2,2,2,2,2,0,0,
  0,0,2,2,1,2,2,0,0,
  0,1,2,1,3,1,2,1,0,
  1,1,1,3,2,3,1,1,1,
  1,1,1,1,2,1,1,1,1,
  1,1,1,1,2,1,1,1,1,
  1,1,1,1,2,1,1,1,1,
  1,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,1,
  1,1,1,1,1,1,1,1,1,
  0,0,3,1,1,1,3,3,1,
  3,2,2,3,1,3,2,3,1,
  3,2,2,2,2,2,2,2,3,
  0,1,2,2,2,2,2,3,0,
  0,1,1,2,2,2,1,1,0,
  0,1,1,3,3,3,1,1,0,
  0,1,1,3,1,3,1,1,0,
  0,1,1,3,1,1,1,1,0,
  0,1,1,1,0,1,1,1,0,
  0,1,1,1,0,1,1,1,0,
  0,1,1,1,3,1,1,1,0,
  0,1,1,1,3,1,1,1,0,
  3,1,1,1,3,1,1,1,3,
  3,1,2,1,3,2,1,1,3,
  3,2,2,2,3,2,2,2,3,
  2,2,2,2,3,2,2,2,2,
  2,2,2,3,3,3,2,2,2,
};
#endif
