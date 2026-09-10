/* timing.h - the original's bout timings, in video frames.
 *
 * $00DF is a frame counter bumped by the vertical blank; the bout code at
 * $2F5F..$3029 spins on it, so these comparisons ARE the durations.
 *
 * An ordinary bout ($00D0 == 1) is timed by the CLOCK and nothing else:
 * $2BA2 ends it when $00DC reaches zero or a fighter reaches four points.
 * The referee's traversal counter $6154 and the tables that drive it belong
 * to $00D0 == 5, a bonus stage this port does not have -- $3972 gates his
 * whole step routine on that state -- so they are not emitted here. An
 * earlier version of this file did emit them, and the port ended every
 * round after eight of his traversals, 13.4 s instead of 30.
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
/* $2D27: the level counts bouts, in BCD, saturating rather than wrapping.
 * $2D09: the AI's skill rises with it, when (level & 3) == 2, and stops at
 * 5. $2C9F: a new match bumps the starting skill and wraps it at 5. */
#define LEVEL_SKILL_STEP 3
#define LEVEL_SKILL_PHASE 2
#endif
