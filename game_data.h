/* game_data.h - the meaning of the game's own tables.
 *
 * The tables themselves live in generated/frames.h, read straight out of a RAM dump by
 * extract_tables.py. They used to be typed out here by hand and were truncated to 21
 * moves and 120 animation frames; the real tables hold 35 moves over 199 frames and
 * reference 54 shapes, so the port was silently missing most of the animation set.
 *
 * Originating addresses, for reference:
 *   $558D  MOVE_FRAME_START   move -> first animation frame
 *   $55B1  FRAME_SHAPE        frame -> shape id
 *   $5679  FRAME_VELX         frame -> signed horizontal velocity
 *   $5740  FRAME_ATTR         frame -> attribute bits (see fighter.h)
 *   $5384  SHAPE_WIDTH        shape -> width in sprite pixels
 *   $6BC0  SHAPE_HEIGHT       shape -> height of the segment-encoded source data
 *   $528F/$52AF/$52CF/$52EF   DISPATCH_R / _L / _altR / _altL
 *   $5287  MOVE_GATE          situation code -> alternate tables allowed
 */
#ifndef GAME_DATA_H
#define GAME_DATA_H

#include <stdint.h>
#include "generated/frames.h"

/* The arena, from the clamp in $5509: a fighter's x is its left edge. */
#define ARENA_LEFT  0x10
#define ARENA_RIGHT 0xAE

/* Move ids, as the dispatch tables produce them (direction + fire). */
enum { AM_STAND=0, AM_JUMP=1, AM_JUMP_F=2, AM_WALK_F=3, AM_DUCK_F=4, AM_CROUCH=5,
       AM_ROLL_B=6, AM_WALK_B=7, AM_JUMP_B=8, AM_ATK_U=9, AM_ATK_UF=10, AM_ATK_F=11,
       AM_ATK_DF=12, AM_ATK_D=13, AM_ATK_DB=14, AM_ATK_B=15, AM_ATK_UB=16,
       AM_HIT=17, AM_FALL=18, AM_KNOCKED=19, AM_SPECIAL=20 };

/* attacks are the fire half of the dispatch */
static inline int am_is_attack(int m){ return (m>=9 && m<=16) || m==20; }

/* --- Scoring: time bonus ($614B, computed at $311E) ---
 * DF = elapsed-time counter when the technique scored.
 *   bonus = (DF >= 0x28) ? 1 : (10 - DF/4)
 * A clean technique is a point toward ippon; scoring faster pays more. */
static inline int score_time_bonus(int df){ return (df>=0x28)?1:(10-(df>>2)); }

#endif
