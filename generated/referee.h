/* referee.h - the referee, captured from the screen.
 *
 * A third figure on the same Player/Missile hardware as the fighters, in his
 * own scanline band. He paces the arena and each turn takes one off the round
 * counter $6154, so he is the round clock as well ($5807).
 * GENERATED FILE - do not edit; re-run extract_referee.py instead. */
#ifndef REFEREE_H_DATA
#define REFEREE_H_DATA
#include <stdint.h>
#define REF_W 9
#define REF_H 32
#define REF_Y 137
#define REF_CLOCKS_PER_PX 2
#define REF_X_ORIGIN 28
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
