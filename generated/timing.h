/* timing.h - the original's bout timings, in video frames.
 *
 * $00DF is a frame counter bumped by the vertical blank; the bout code at
 * $2F5F..$3029 spins on it, so these comparisons ARE the durations.
 *
 * The round length is not a clock: $6154 counts REFEREE TRAVERSALS. The
 * referee's x ($6159) starts at $28 or $DC and steps by $615A each frame
 * ($58AD, normally 2) until it passes $F0 or falls under $0A, and each turn
 * decrements $6154 ($5834). $2F3A gives the number of traversals per round.
 * GENERATED FILE - do not edit. */
#ifndef TIMING_H
#define TIMING_H
#include <stdint.h>
/* $2F9A: hold before the fighters are shown */
#define T_READY   0x50
/* $2FA9: hold until the bout is enabled ($615D) */
#define T_BEGIN   0xC8
/* $2FEA and $3017: the freeze after a point, and after time runs out */
#define T_FREEZE  0x80
/* $2F7C: both fighters are placed at this x when a bout starts */
#define T_START_X 0x54
/* $2F3A, indexed by the round number $615F */
static const uint8_t ROUND_TRAVERSALS[3]={8,15,20};
/* The referee does NOT move. $6159 is how far through one of his signalling
 * actions he is: $58EF starts one, setting $6159 to $28 or $DC and $615A to a
 * step from $58AD, and $5807 walks it to $F0 or $0A. Reaching the end is what
 * decrements the round counter $6154 ($5834), so a round is a number of his
 * actions, not of anything geometric. $58A1 picks which of three actions, by
 * round number and a random draw. */
static const uint8_t REF_ACTION[12]={0,1,2,2,0,1,2,0,0,1,2,2};   /* $58A1 */
static const uint8_t REF_STEP[12]={2,2,2,2,2,2,2,2,2,1,2,3};     /* $58AD */
static const uint8_t REF_SIDE[12]={1,1,0,1,0,0,1,0,1,1,0,1};     /* $5885 */
#define REF_END_LOW   0x0A
#define REF_END_HIGH  0xF0
#define REF_START_UP   0x28
#define REF_START_DOWN 0xDC
/* $3BF9: the main loop only takes a game tick once the vertical blank has
 * counted more than this many frames into $6121, so the fighters advance at
 * one tick per divider+1 video frames -- 10 Hz at the default setting, not 60.
 * $619B selects the entry; a hidden key sequence changes it ($33C3). */
static const uint8_t GAME_TICK_DIVIDER[4]={5,4,6,7};
#define GAME_TICK_DEFAULT 0
/* $3C01: while the referee has play frozen ($00D8) the divider is this */
#define GAME_TICK_FROZEN 6
/* $00DC: the clock the HUD shows. BCD, decremented once every $3C frames
 * by $3988, and started at $30 for one player or $60 for two. */
#define T_CLOCK_1P 0x30
#define T_CLOCK_2P 0x60
#define T_CLOCK_TICK 0x3C
#endif
