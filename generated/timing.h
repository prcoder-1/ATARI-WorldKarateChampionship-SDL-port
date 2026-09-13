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
/* $2903: how long the game stays in state 2 after a scoring blow. BOTH
 * fighters run for it ($28F7's $277A -> $51DC), so each finishes its move
 * and is then put back to stand by $530F, which imposes $6131 while $00D8
 * is set. Measured on the machine: the fighter that did not take the blow
 * is standing again by frame 28 of the 150. */
#define T_POINT   0x96
/* $2FEA and $3017: the bonus stage's own freeze, which drives ONE fighter */
#define T_FREEZE  0x80
/* $2F7C: both fighters are placed at this x when a bout starts */
#define T_START_X 0x54
/* The ceremony.
 *
 * $2DC0, in the reset that opens a bout ($2D89, which also runs after every
 * scoring blow): both fighters are handed move $1C, the bow. Always in a
 * one-player game; with two people playing only while the clock is still
 * full, so it opens a bout without interrupting one.
 *
 *   2DC0: LDX #$1C          the bow
 *   2DC2: LDA $50 / AND $51 both human?
 *   2DC6: BEQ $2DD0         no -- bow
 *   2DC8: LDA $DC / CMP #$60 / BCS $2DD0   yes -- only on a full clock
 *   2DCE: LDX #$00          otherwise just stand
 *   2DD1: STA $00EC,Y / JSR $2783 ($530F)  start it
 *
 * $29AD, once a bout is over: the winner is not driven through the move but
 * posed by hand, his shape written straight into $00DD,X. He goes down, is
 * held there, and comes back up; each pose also steps him back one
 * ($29B1/$29D6). When the bout ended on the clock rather than on points,
 * $292F stands BOTH fighters up first.
 *
 *   292F: LDA #$30 / STA $DD / STA $DE     both upright (clock only)
 *   29AD: LDA #$31 / STA $DD,X            the winner bends
 *   29BB: LDX #$19 / JSR $2F30            hold
 *   29C6: LDX #$32 / JSR $2F30            hold
 *   29D2: LDA #$30 / STA $DD,X            and straightens
 *   29DE: LDX #$19 / JSR $2F30            hold
 *
 * $1C's own frames run 48, 49, 48 -- the same two shapes -- so the pose the
 * winner is put into by hand is the one the move would have reached. */
#define MOVE_BOW  0x1C
#define T_BOW_2P_CLOCK 0x60
#define SHAPE_BOW_UP   48
#define SHAPE_BOW_DOWN 49
/* $292F stands both fighters in the same pose the winner returns to */
#define SHAPE_BOUT_END 48
#define T_BOW_DOWN 75   /* $29BB + $29C6: bent, then held */
#define T_BOW_UP   25   /* $29DE: and upright before the game moves on */
/* $2A1B: the last hold, after the ceremony and the time bonus, before the
 * game decides between another bout and the high-score screen */
#define T_BOUT_TAIL 50
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
