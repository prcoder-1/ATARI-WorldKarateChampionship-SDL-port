/* World Karate Championship - Linux/SDL2 port
 *
 * The fighter logic here is the ORIGINAL's, ported from the 6502: see fighter.h.
 * A fighter's state is an index into the game's own 120-entry per-frame tables, and
 * FRAME_ATTR / FRAME_VELX / FRAME_SHAPE drive everything. There is no invented move
 * table, no tuned durations and no jump physics -- the original has none.
 *
 * Graphics are the original's own data:
 *   generated/scenes.h    all seven stages, ANTIC mode E, pixel-exact
 *   generated/shapes_pm.h all 47 fighter poses, in colour, with their vertical placement
 *
 * The logical screen IS the Atari frame: 384 colour clocks by 240 scanlines, so every
 * recovered coordinate is used as-is and a port frame can be diffed against an emulator
 * capture directly.
 *
 * Controls
 *   Player 1 : W/A/S/D,  Left Shift = fire (attack modifier)
 *   Player 2 : Arrow keys, Right Shift = fire  (or CPU, toggle with C)
 *   Space = start / next,  P = pause,  Esc = quit
 */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

#include "game_data.h"            /* the ROM's dispatch / animation / scoring tables */
#include "fighter.h"              /* the ROM's fighter state machine                */
#include "ai.h"                   /* the ROM's CPU opponent ($3D04)                  */
#include "generated/shapes_pm.h"  /* the game's own 47 poses, in colour              */
#include "atari_gfx.h"            /* ANTIC mode E playfield + the recovered scenes    */
#include "pokey.h"                /* the game's music player ($1F06) and POKEY tones  */
#include "sfx.h"                  /* the game's digitised sound effects ($3A7C/$3AE0) */
#include "hud.h"                  /* the game's HUD: its font, colours and layout      */
#include "generated/referee.h"    /* the referee, captured from the screen             */
#include "generated/signs.h"      /* the signs he holds up, captured the same way      */
#include "hit_test.h"             /* $415D: whether a blow landed                      */
#include "generated/popup.h"      /* the points a blow scores, put up on the ground    */
#include "hiscore.h"              /* $6220..$624F: the table, and $60B1's sort         */
#include "generated/timing.h"     /* the bout's frame counts, from $2F5F..$3029       */

/* ------- the logical screen is the Atari frame -------
 * 384 colour clocks by 240 scanlines. SCALE is how many window pixels one of those
 * units becomes, and it is the only place the output size is decided: everything below
 * draws in Atari units and fillrect() multiplies. Each unit is emitted as a solid
 * SDL_RenderFillRect on integer boundaries, so there is no filtering or resampling at
 * any scale -- one unit is a hard-edged SCALE x SCALE block. */
#define LW 384
#define LH 240
#define SCALE 4
#define WINW (LW*SCALE)
#define WINH (LH*SCALE)
#define FPS 60

/* game x (a sprite's left edge, arena units $10..$AE) -> colour clocks.
 * Calibrated against captures: $E1 = 140 put the sprite's left edge at clock 308. */
#define FX_SCALE 2
#define FX_ORIGIN 28
#define FGT_X(gx) (FX_ORIGIN + (gx) * FX_SCALE)

/* ---------------- Atari-style palette ---------------- */
typedef struct { Uint8 r,g,b; } Col;
static Col rgb(Uint8 r,Uint8 g,Uint8 b){ Col c={r,g,b}; return c; }

/* ---------------- audio ----------------
 * Two independent sources, exactly as the original has them, and they do not mix: an
 * effect sets the player's mute flag $CF, so the music drops out for its duration and
 * comes back afterwards.
 *
 * The enable flags are the game's own: $0054 music, $0055 effects. The attract demo
 * runs with music on and effects off; starting a game ($3337) turns effects on and the
 * music off. Either can be toggled from the keyboard. */
#define SR 44100
static SDL_AudioDeviceID audio=0;
static Music music;
static PokeyOsc pokeyOsc;
static Sfx sfx;
static int musicOn=1;             /* $0054 */
static int sfxOn=0;               /* $0055 */
static int musSamples;            /* samples left in the current video frame */

/* The music clock lives here so it stays locked to the audio, exactly as the original
 * ties it to the vertical blank: musicTick() once per video frame, SR/FPS samples apart. */
static void audioCB(void* u,Uint8* stream,int len){
    (void)u;
    int16_t* out=(int16_t*)stream; int count=len/2;
    memset(out,0,(size_t)len);
    for(int i=0;i<count;){
        if(musSamples<=0){
            if(musicOn) musicTick(&music);     /* $3941: the VBI skips it when $54 = 0 */
            musSamples=SR/FPS;
        }
        int n=count-i; if(n>musSamples) n=musSamples;
        if(sfxActive(&sfx))      sfxRender(&sfx,out+i,n,SR);   /* $CF mutes the music */
        else if(musicOn)         pokeyRender(&pokeyOsc,&music,out+i,n,SR);
        i+=n; musSamples-=n;
    }
}

/* ---------------- global game state ---------------- */
/* The game boots into the demo -- a bout it plays against itself, $50 and $51 both zero
 * -- and START or SELECT takes over. The two screens either side of it are the port's
 * arrangement of the original's pieces: G_INTRO shows the high-score table with the key
 * bindings under it before the demo starts, and a lost match goes MATCH OVER -> the
 * table -> back to the demo, which is the cycle $2A35/$2A42 set up ($D0 = 6 -> $5C8C). */
enum { G_INTRO, G_FIGHT, G_POINT, G_ROUND_END, G_MATCH_END, G_NAME, G_HISCORE };
static int gstate=G_INTRO;
#define T_INTRO   (30*FPS)      /* the instruction screen, thirty seconds */
#define T_HISCORE (10*FPS)
static int stateTimer=0;
static Fighter p1,p2;
static int roundClock;     /* $00DC: the BCD seconds the HUD shows */
static int tickCounter;    /* $6121: video frames since the last game tick */
static unsigned frameCount; /* $0014: the clock the blink comes off ($5E68) */
static int updateParity;   /* $6115: which fighter is driven first this tick */
/* $00D8 freezes play after a blow. Two things follow, and the port did neither:
 *   - $51F1 runs the state machine for the fighter named by $D1 ONLY, and skips the
 *     joystick read entirely ($51EE is the entry that reads sticks; $274A jumps past
 *     it). The attacker holds its pose.
 *   - $530F's first branch overrides the queued move with $6131 while $D8 is set, so
 *     the struck fighter cannot pick up whatever was last asked for.
 * Without them the attacker kept animating from a queue nothing was refreshing, and a
 * move still queued when the blow landed repeated for the whole freeze -- releasing the
 * keys did nothing, because in that state no key is read. */
static Fighter* freezeWho;
static int freezeMove;
static int speedIndex=GAME_TICK_DEFAULT;  /* $619B: which divider is in use */
static int clockTick;      /* $6141: frames until the next second */
/* The referee's action machinery ($6159/$615A/$615B/$615E, started by $58EF and stepped
 * by $5807) is deliberately absent. $3972 gates all of it on $D0 == 5, a bonus stage
 * this port does not have; in an ordinary bout ($D0 == 1) he never starts or steps an
 * action, so his round counter $6154 never moves. The port used to run it every video
 * frame and end the round after eight traversals -- 13.4 s, always beating the 30 s
 * clock. What ends a bout is $2BA2: the clock reaching zero, or four points. */
static int bg=0;
/* $00D3: the level, in BCD, counting BOUTS. $2D27 adds one and $2D2E's BCS drops the
 * store on carry, so it saturates at $99 instead of wrapping. The HUD shows it as
 * "L nn" ($5C2A). */
static int level=0;
/* $6150: bouts since the last bonus stage. $2D57 compares it with 8. */
static int boutCount=0;
/* $2A26: the match runs only while fighter 0 keeps winning. The moment it loses a bout
 * $2A35 sets $6168 to the loser and $D0 to 6, which is the high-score screen. */
static int boutLost=0;
/* $00D9/$00DA, $F7..$F9/$FA..$FC and the two-player win markers are MATCH state, not
 * fighter state. The ROM keeps them well away from the per-fighter record and clears the
 * scores only when a new match starts ($2C90). The port used to hold them inside Fighter,
 * where placeFighter's memset wiped them and resetPositions restored the points and the
 * wins by hand -- but not the score. resetPositions runs after every scoring blow, so the
 * score went back to zero each time and a match always ended with nothing for the table. */
static int fPoints[2];   /* $00D9,y */
static int fScore[2];    /* $F7..$F9 and $FA..$FC */
static int fWins[2];

/* $5DE0: which row of the table the loser is putting a name into, and the entry
 * itself (hiscore.h). In the original the stick picks the letters and the button
 * takes them; here the movement keys do the picking. */
static int nameRow=-1;
static HsName nameEntry;
#define SCENE_EVERY 8
/* $2D09: the skill stops here and never falls back within a match */
#define AI_SKILL_MAX 5
static int bcdInc(int v)
{
    int lo=(v&0x0F)+1, hi=(v>>4)&0x0F;
    if(lo>9){ lo=0; hi++; }
    if(hi>9) return v;                 /* $2D2E: the carry is not stored */
    return (hi<<4)|lo;
}
static char banner[64]="";
/* $615C picks one of the referee's three actions; what the player actually reads is the
 * sign he holds up beside him. The bitmaps are captured (generated/signs.h) and named
 * there; the port asks for one by name so a missing sign fails visibly rather than
 * silently drawing the wrong board. */
/* $613F and $6140: which points-scored number to put up, and where. The hit test has
 * always worked both out ($4044/$404C and $42DE); until now the port dropped them and
 * drew an invented "BONUS n" banner instead. */
static int popupIndex=-1, popupColumn=0;
static int signIndex=-1;
static int signTimer;

static int signByName(const char* want)
{
    for(int i=0;i<SIGN_COUNT;i++)
        if(!strcmp(SIGN_NAME[i],want)) return i;
    return -1;
}
static void signShow(const char* want,int frames)
{
    signIndex=signByName(want); signTimer=frames;
}
static int paused=0;
static int p2isCPU=1;
/* $0050/$0051: the demo is simply the state where BOTH are zero. $3312 sets them when
 * the console keys are pressed -- START gives $50=1,$51=0 and SELECT $50=1,$51=1 -- and
 * until then the game plays itself with the music on and the effects off. */
static int p1isCPU=1;
static int aiSkill=1;   /* $F6: the ROM's skill level, 1..5, rising with the DAN rank */
static unsigned rng=0x1234;
static unsigned rnd(void){ rng=rng*1103515245u+12345u; return (rng>>16)&0x7fff; }

#define NSCENES BG_SCENE_COUNT

static SDL_Renderer* R;
static void setcol(Col c){ SDL_SetRenderDrawColor(R,c.r,c.g,c.b,255); }
static void fillrect(int x,int y,int w,int h){ SDL_Rect r={x*SCALE,y*SCALE,w*SCALE,h*SCALE}; SDL_RenderFillRect(R,&r);}
static void setpx1(int x,int y,int r,int g,int b){ setcol(rgb((Uint8)r,(Uint8)g,(Uint8)b)); fillrect(x,y,1,1); }

/* ---------------- backgrounds ---------------- */
static void drawBackground(void){ sceneDraw(&BG_SCENES[bg%NSCENES], LW, LH, setpx1); }

/* ---------------- fighters ----------------
 * A pose is drawn at its own absolute top scanline (no vertical motion exists) and at
 * two colour clocks per sprite pixel, mirrored when the fighter faces left. */
/* The colours come from the captures via generated/shapes_pm.h, not from taste: the
 * port had been painting the skin tan (230,180,150) where the game puts a pink
 * (189,113,121), the white gi brighter than it is, and the outline pure black. */
static const Col COL_SKIN    = {SHAPE_COL_SKIN};
static const Col COL_OUTLINE = {SHAPE_COL_OUTLINE};
static const Col COL_GI_P1   = {SHAPE_COL_GI_WHITE};
static const Col COL_GI_P2   = {SHAPE_COL_GI_RED};

/* ShapePM.x0 is where the pose's ink starts, in colour clocks from the fighter's
 * origin, as captured from the fighter that faces LEFT. Facing right the pose is
 * mirrored within the same Player/Missile field -- four adjacent double-width Players,
 * 64 colour clocks -- so its ink starts at SHAPE_MIRROR_CLOCKS - x0 - 2*w instead.
 *
 * That constant is measured by extract_sprites.py from the left-hand fighter, which
 * faces right, and 83 of 84 captures agree on it. It used to be derived instead, from
 * $5509's clamp box being $24 = 36 game units wide, which put it 56 clocks out and drew
 * every right-facing fighter 28 sprite pixels too far right. */
static int spriteLeft(const ShapePM* s, const Fighter* f)
{
    int ink = s->x0;
    if(!f->facing) ink = SHAPE_MIRROR_CLOCKS - s->x0 - s->w*FX_SCALE;
    return FGT_X(f->x) + ink;
}

static void drawFighter(const Fighter* f, Col gi)
{
    const ShapePM* s=&SHAPE_PM[f->shape % SHAPE_COUNT];
    if(!s->h || !s->px) return;
    int left=spriteLeft(s,f);
    for(int y=0;y<s->h;y++){
        for(int k=0;k<s->w;k++){
            uint8_t v=s->px[y*s->w+k];
            if(!v) continue;
            int col = f->facing ? (s->w-1-k) : k;   /* stored facing right */
            Col c = (v==SHAPE_IDX_GI)?gi : (v==SHAPE_IDX_SKIN)?COL_SKIN : COL_OUTLINE;
            setcol(c);
            fillrect(left+col*FX_SCALE, s->y0+y, FX_SCALE, 1);
        }
    }
}

/* ---------------- the referee ----------------
 * He does not move. An earlier version of this port had him pacing the arena, on a
 * misreading of $6159 as his position; it is the progress of one of his signalling
 * actions ($58EF starts one, $5807 walks the counter, $5834 ends it). His objects sit at
 * fixed positions taken from $58D1, and over 187 captured frames he is in the same place
 * in 181 of them. He is drawn where he stands. */
/* The sign he holds up. It is a Player/Missile object like he is, always the same size
 * and always in the same place beside him; index 0 in a captured sign is the ground
 * showing through its cut corner. */
/* $4660/$46F0: the points a blow scored, four characters on the ground at the column
 * $6140 worked out from the attacker's position. Pixel value 3 is COLPF3 ($0F, white)
 * and value 1 COLPF0 ($00, black); 0 and 2 let the ground through. */
static void drawPopup(void)
{
    if(popupIndex<0 || popupIndex>=POPUP_COUNT) return;
    static const Col ink={POPUP_COL_INK}, shadow={POPUP_COL_SHADOW};
    int left = POPUP_LEFT + popupColumn * POPUP_PX_PER_CHAR * POPUP_CLOCKS_PER_PX;
    for(int y=0;y<POPUP_LINES;y++)
        for(int x=0;x<POPUP_CHARS*POPUP_PX_PER_CHAR;x++){
            uint8_t v=POPUP_PX[popupIndex][y][x];
            if(v!=1 && v!=3) continue;
            setcol(v==3?ink:shadow);
            fillrect(left+x*POPUP_CLOCKS_PER_PX, POPUP_TOP+y, POPUP_CLOCKS_PER_PX, 1);
        }
}

static void drawSign(void)
{
    if(signIndex<0 || signIndex>=SIGN_COUNT) return;
    static const Col board={SIGN_COL_BOARD}, ink={SIGN_COL_INK};
    for(int y=0;y<SIGN_H;y++)
        for(int k=0;k<SIGN_W;k++){
            uint8_t v=SIGN_PX[signIndex][y*SIGN_W+k];
            if(!v) continue;
            setcol(v==SIGN_IDX_BOARD?board:ink);
            fillrect(SIGN_X+k*SIGN_CLOCKS_PER_PX, SIGN_Y+y, SIGN_CLOCKS_PER_PX, 1);
        }
}

static void drawReferee(void)
{
    if(gstate!=G_FIGHT && gstate!=G_POINT && gstate!=G_ROUND_END) return;
    for(int y=0;y<REF_H;y++)
        for(int k=0;k<REF_W;k++){
            uint8_t v=REF_PX[y*REF_W+k];
            if(!v) continue;
            Col c = (v==SHAPE_IDX_GI)?COL_GI_P1
                  : (v==SHAPE_IDX_SKIN)?COL_SKIN : COL_OUTLINE;
            setcol(c);
            fillrect(REF_X+k*REF_CLOCKS_PER_PX, REF_Y+y, REF_CLOCKS_PER_PX, 1);
        }
}


/* ---------------- input ----------------
 * Produces an Atari stick nibble (ACTIVE LOW: bit0 up, 1 down, 2 left, 3 right)
 * plus the trigger, exactly what $51F9 reads from PORTA/TRIG. */
typedef struct { int stick, fire; } Stick;

static Stick readKeys(const Uint8* k, bool p2keys)
{
    Stick s={0x0F,0};
    if(p2keys){
        if(k[SDL_SCANCODE_UP])    s.stick&=~0x01;
        if(k[SDL_SCANCODE_DOWN])  s.stick&=~0x02;
        if(k[SDL_SCANCODE_LEFT])  s.stick&=~0x04;
        if(k[SDL_SCANCODE_RIGHT]) s.stick&=~0x08;
        s.fire=k[SDL_SCANCODE_RSHIFT];
    } else {
        if(k[SDL_SCANCODE_W]) s.stick&=~0x01;
        if(k[SDL_SCANCODE_S]) s.stick&=~0x02;
        if(k[SDL_SCANCODE_A]) s.stick&=~0x04;
        if(k[SDL_SCANCODE_D]) s.stick&=~0x08;
        s.fire=k[SDL_SCANCODE_LSHIFT];
    }
    return s;
}

/* ---------------- text, in the game's own character set ----------------
 * The port used to carry an invented 5x7 font. Now that the game's charset is
 * extracted, banners and the title use it too: value-3 pixels in the requested colour,
 * everything else transparent. */
static void gameChar(int x,int y,int c,Col col)
{
    const uint8_t* g=HUD_FONT[c & 0x7F];
    for(int line=0;line<8;line++)
        for(int k=0;k<4;k++)
            if(((g[line]>>(6-2*k))&3)==3){
                setcol(col);
                fillrect(x+k*2,y+line,2,1);
            }
}

static void drawText(int x,int y,const char* t,Col col)
{
    for(;*t;t++,x+=8){
        if(*t==' ') continue;
        int c = (*t>='0'&&*t<='9') ? HUD_CH_DIGIT0+(*t-'0')
              : (*t>='A'&&*t<='Z') ? HUD_CH_LETTER_A+(*t-'A')
              : (*t>='a'&&*t<='z') ? HUD_CH_LETTER_A+(*t-'a') : -1;
        if(c>=0) gameChar(x,y,c,col);
    }
}
static void drawTextC(int y,const char* t,Col col){ drawText((LW-(int)strlen(t)*8)/2,y,t,col); }

/* The ground below the playfield is twelve ANTIC mode 4 rows ($62CC, LMS $0800). Its
 * top comes from the points popup, which the ROM puts in row 4 and which was measured on
 * screen at scanline POPUP_TOP. */
#define GROUND_ROW_LINES 8
#define GROUND_TOP  (POPUP_TOP - 4*GROUND_ROW_LINES)
#define GROUND_LEFT POPUP_LEFT
#define GROUND_CHAR (POPUP_PX_PER_CHAR*POPUP_CLOCKS_PER_PX)

/* the same, from a string, centred across the forty columns */
static void groundTextC(int row,const char* t,Col col)
{
    int n=(int)strlen(t);
    int x=GROUND_LEFT+((40-n)/2)*GROUND_CHAR;
    drawText(x, GROUND_TOP+row*GROUND_ROW_LINES, t, col);
}

static void groundText(int row,int col,const uint8_t* codes,int n,Col c)
{
    for(int i=0;i<n;i++)
        gameChar(GROUND_LEFT+(col+i)*GROUND_CHAR, GROUND_TOP+row*GROUND_ROW_LINES,
                 codes[i], c);
}

/* $5FE5 and $6013: the header at row 4 column 10, then one row per entry from row 6 --
 * position, name, belt and a six-digit score with the leading zeros blanked but never
 * the last ($607B/$6061). */
static void drawHiScore(void)
{
    static const Col ink={SHAPE_COL_GI_WHITE};
    groundText(HS_HEADER_ROW, HS_COL, HS_HEADER, HS_HEADER_LEN, ink);
    for(int i=0;i<HS_ENTRIES;i++){
        const HsRow* r=&hsTable[i];
        int row=HS_ROW0+i;
        uint8_t pos=(uint8_t)(i+1);                       /* $6017 */
        groundText(row, HS_COL+HS_OFF_POS, &pos, 1, ink);
        if(i==nameRow){
            /* $5E66: the characters already taken, then the one being chosen, blinking
             * off $14 -- blank for eight frames in every thirty-two. */
            groundText(row, HS_COL+HS_OFF_NAME, nameEntry.buf, HS_NAME_LEN, ink);
            uint8_t cur = (frameCount & 0x18) ? (uint8_t)nameEntry.ch : HUD_CH_SPACE;
            if(nameEntry.pos < HS_NAME_LEN)
                groundText(row, HS_COL+HS_OFF_NAME+nameEntry.pos, &cur, 1, ink);
        } else
            groundText(row, HS_COL+HS_OFF_NAME, r->name, HS_NAME_LEN, ink);
        if(hsHasBelt(r))                                  /* $6035/$603D */
            groundText(row, HS_COL+HS_OFF_BELT, HS_BELT[r->belt%6], HS_BELT_LEN, ink);
        uint8_t dig[6]; int sc=hsScoreOf(r), div=100000, lead=1;
        for(int k=0;k<6;k++,div/=10){
            int v=(sc/div)%10;
            if(v||k==5) lead=0;
            dig[k]= lead ? HUD_CH_SPACE : (uint8_t)v;
        }
        groundText(row, HS_COL+HS_OFF_SCORE, dig, 6, ink);
    }
}

/* ---------------- HUD ---------------- */
static uint8_t hudCells[HUD_CELLS];

static void drawHUD(void)
{
    int demo = !(p1.isHuman || p2.isHuman);
    /* $5C32/$5C3F: the two digits of $00D3 straight out, already BCD. The belt is no
     * longer passed: $5F66 derives it from the score. */
    hudCompose(hudCells, p1.isHuman, p2.isHuman, demo, roundClock,
               level, fScore[0], fScore[1],
               fWins[0], fWins[1], 0);
    hudDraw(hudCells, LW, LH, setpx1);
    hudMarkers(fPoints[0], fPoints[1], p1.isHuman, p2.isHuman, LW, LH, setpx1);
}

/* ---------------- round / match flow ---------------- */
static void placeFighter(Fighter* f,int x,int facing,int index)
{
    memset(f,0,sizeof *f);
    f->x=x; f->facing=facing; f->index=index;   /* $D2 */
    f->move=0; f->frame=f->next=MOVE_FRAME_START[0];
    f->shape=FRAME_SHAPE[f->frame];
}

static void resetPositions(void)
{
    int cpu=p2.isCPU;
    /* $2F7C places both fighters at $54. Nothing that has to outlive a bout lives in
     * here any more, so there is nothing to save across the wipe but isCPU. */
    placeFighter(&p1,T_START_X-0x20,0,0);   /* faces right */
    placeFighter(&p2,T_START_X+0x20,1,1);   /* faces left  */
    p2.isCPU=cpu;
    /* $0050,y: nonzero = joystick, zero = the CPU routine */
    p1.isHuman=!p1isCPU;
    p2.isHuman=!p2.isCPU;
}

/* The attract state: the game plays itself. $50 and $51 are both zero, so $3D04 drives
 * both fighters and $51F9 reads nothing; the music runs and the effects are off, which
 * is the mirror of what $3337 sets up when a game starts. */
static void demoStart(void);

static void newBout(void)
{
    fPoints[0]=fPoints[1]=0;
    /* $2D27: the level counts BOUTS, in BCD, and saturates rather than wrapping
     * ($2D2E's BCS skips the store). $2D09: the skill rises with it, when
     * (level & 3) == 2, and stops at 5 -- it does not fall back. */
    level = bcdInc(level);
    if(aiSkill < AI_SKILL_MAX && (level & 3) == 2) aiSkill++;
    /* $2D32 counts bouts in $6150; $2D57 sends the ninth to the bonus stage, and it is
     * the END of that stage that advances the scene ($3043 -> $270B -> $438C: the next
     * of seven, wrapping). This port has no bonus stage, so it advances the scene on the
     * same bout count instead -- the timing the ROM gives it, without the stage. */
    if(++boutCount > SCENE_EVERY){ boutCount=0; bg=(bg+1)%NSCENES; }
    /* $2D25/$2D3D: 30 seconds against the computer, 60 with two players */
    roundClock = p2.isCPU ? T_CLOCK_1P : T_CLOCK_2P;
    clockTick = T_CLOCK_TICK;
    tickCounter = 0;
    signShow("BEGIN",T_BEGIN);
    boutLost=0;
    resetPositions();
    gstate=G_FIGHT;
   }

/* $2FFA/$3004: a scoring blow freezes play and forces the loser into move 17 or 18,
 * chosen by whether the blow came from the front or from behind. */
/* $4502: $00D9,y takes the blow's weight -- two for a solid one, one for a glancing --
 * and saturates at 5. $4550 adds $6132 to the fighter's BCD score. */
static int bcd(int v){ return (v >> 4) * 10 + (v & 0x0F); }

static void award(Fighter* a, const HitResult* h)
{
    Fighter* d = (a==&p1)?&p2:&p1;
    int who = a->index & 1;
    fPoints[who] += h->hit;                       /* $4508 */
    if(fPoints[who] > HIT_POINTS_MAX) fPoints[who] = HIT_POINTS_MAX;   /* $450B */
    fScore[who] += bcd(h->score) * 100;           /* $4550, in BCD */
    /* $2DFB: LDA $613F / JSR $46F0 / JSR $4660 -- the number and its column, straight
     * from the blow. It is the points scored: 100, 200, 400, 500, 800 or 1600. */
    popupIndex = h->sign;
    popupColumn = h->column;
    /* the game's own wording, off its signs. $613F names the announcement the ROM puts
     * up, but what it indexes has not been established, so the sign is still chosen by
     * the blow's weight rather than by that number. */
    signShow(h->hit>=2 ? "FULL POINT" : "HALF POINT", T_FREEZE);
    strcpy(banner, "");
    gstate=G_POINT; stateTimer=T_FREEZE;          /* $2FEA: 128 frames */
    /* $42D8/$28A5: the move forced on the struck fighter comes from $4062, keyed by the
     * strike and the pair of facings -- not from a guess about which side it came from */
    d->queued = h->reaction;
    d->samemove = 0;
    fgtStartMove(d, h->reaction, rnd);            /* $3011/$2747 */
    freezeWho = d;                                /* $2FFE */
    freezeMove = 0;                               /* $6131 is 0 here ($289D/$2D9F) */
}

/* ---------------- one frame of a fight ---------------- */
/* The ROM's frame ($51C0): read both joysticks, then for each fighter run the CPU
 * routine followed by that fighter's state machine. */
/* $399D runs every frame, whatever the state. $00D0 is the game state: 1 while
 * fighting, 2 and 5 for the states that use the third effect table; $00D8 is the
 * referee's freeze flag, which is what the announcement sounds key off. */
static void soundFrame(void)
{
    int shapes[2]={p1.shape,p2.shape};
    int attrs[2]={p1.attr,p2.attr};
    int state = (gstate==G_FIGHT||gstate==G_POINT) ? 1 : 2;
    int frozen = (gstate==G_POINT);
    SDL_LockAudioDevice(audio);
    sfxFrame(&sfx,shapes,attrs,state,frozen,sfxOn);
    SDL_UnlockAudioDevice(audio);
}

/* $3BF9: the main loop takes a game tick only once the vertical blank has counted more
 * than GAME_TICK_DIVIDER frames into $6121 -- so the fighters advance at one tick per
 * divider+1 video frames, about 10 Hz, while video, music and the clock stay at 60. The
 * divider grows to GAME_TICK_FROZEN while the referee has play frozen ($00D8). */
static int gameTickDue(int frozen)
{
    int div = frozen ? GAME_TICK_FROZEN : GAME_TICK_DIVIDER[speedIndex & 3];
    tickCounter++;
    if (tickCounter > div) { tickCounter = 0; return 1; }
    return 0;
}

/* one game tick: input, the CPU opponent, both state machines, and scoring */
static void fightTick(const Uint8* keys)
{
    /* $51C3: $D2 takes $6115, which $3C62 toggles every tick, so which fighter is
     * driven first alternates from tick to tick. */
    Fighter* order[2];
    order[0] = updateParity ? &p2 : &p1;
    order[1] = updateParity ? &p1 : &p2;
    updateParity ^= 1;

    /* $51F9. The alternate (close-quarters) dispatch tables come in when the fighters
     * are close, the opponent's input gate is open, and MOVE_GATE allows it here. */
    for(int i=0;i<2;i++){
        Fighter* f=order[i]; Fighter* o=order[i^1];
        if(!f->isHuman) continue;
        FgtGeom g = fgtGeometry(f,o);
        int alt = (g.quarter < 8) && o->gate && MOVE_GATE[g.code & 7];
        Stick s = readKeys(keys, f==&p2);
        f->queued = fgtDispatch(f, s.stick, s.fire, alt);
    }

    for(int i=0;i<2;i++){
        Fighter* f=order[i]; Fighter* o=order[i^1];
        if(!f->isHuman){
            /* $3D04, then $3E32: STA $EC,y / JSR $530F / clear $EC,y. The move has to
             * go in through the queue, because $530F's queued branch also re-arms the
             * idle counter ($535A) -- forcing the move instead left the CPU fighter's
             * idle counter running until it fired off taunts mid-fight. */
            int m = aiChooseMove(f, o, aiSkill, rnd);
            if(m >= 0){ f->queued = m; fgtStartMove(f, -1, rnd); f->queued = 0; }
        }
        fgtUpdate(f,-1,rnd);
    }

    /* Scoring. The original reads a GTIA collision register in the VBI ($3888) to set
     * its hit flag; that path is not modelled, so the port uses the equivalent test in
     * software: an attacking fighter whose sprite overlaps the opponent's connects. */
    /* $288A: JSR $2729 -> $415D, once per tick, after both fighters have moved and
     * after $3BF9 has toggled $6115 -- which is why the parity handed in is the one
     * this tick has already flipped to. */
    {
        const Fighter* pair[2] = { &p1, &p2 };
        HitResult h = hitTest(pair, updateParity);
        if(h.hit) award(h.attacker ? &p2 : &p1, &h);
    }
}

/* one video frame, whatever the game state: everything the original does from its
 * vertical blank -- the clock, the referee, the sound watcher */
static void vblank(void)
{
    if(gstate==G_FIGHT){
        /* $3988: a BCD decrement once a second. $3981 holds it while fighter 0 is in
         * move $1C -- the bow -- so the ceremony does not eat the round. */
        if(--clockTick<=0){
            clockTick=T_CLOCK_TICK;
            if(roundClock && p1.move!=MOVE_BOW){
                int lo=roundClock&0x0F, hi=roundClock>>4;
                if(lo) lo--; else { lo=9; if(hi) hi--; }
                roundClock=(hi<<4)|lo;
            }
        }
        /* $2BA2: the bout is over when the clock reaches zero, or when either fighter
         * has four points. Nothing else ends it -- the referee does not time it. */
        if(roundClock==0){
            gstate=G_ROUND_END; stateTimer=T_BEGIN;
            /* the signs name the fighters by their gi, RED and WHITE */
            if(fPoints[0]>fPoints[1]){ fWins[0]++; signShow("WHITE",T_BEGIN); strcpy(banner,""); }
            else if(fPoints[1]>fPoints[0]){ fWins[1]++; boutLost=1; signShow("RED",T_BEGIN); strcpy(banner,""); }
            else { signIndex=-1; strcpy(banner,"DRAW"); }
        }
    }
    if(signTimer>0 && --signTimer==0) signIndex=-1;
    soundFrame();
}

static void demoStart(void)
{
    p1isCPU=1; p2isCPU=1; p2.isCPU=1;
    boutLost=0;
    bg=0; boutCount=0; level=0; aiSkill=1;
    fWins[0]=fWins[1]=0; fScore[0]=fScore[1]=0;
    musicOn=1; sfxOn=0; musicPlay(&music);
    popupIndex=-1;
    newBout();
}

int main(int argc,char**argv)
{
    (void)argc;(void)argv;
    if(SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO)!=0){ SDL_Log("init: %s",SDL_GetError()); return 1; }
    SDL_Window* win=SDL_CreateWindow("World Karate Championship (SDL port)",
        SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,WINW,WINH,0);
    R=SDL_CreateRenderer(win,-1,SDL_RENDERER_ACCELERATED|SDL_RENDERER_PRESENTVSYNC);
    if(!R){ SDL_Log("renderer: %s",SDL_GetError()); return 1; }

    SDL_AudioSpec want,have; SDL_zero(want);
    want.freq=SR; want.format=AUDIO_S16SYS; want.channels=1; want.samples=512; want.callback=audioCB;
    audio=SDL_OpenAudioDevice(NULL,0,&want,&have,0);
    hsReset();
    musicInit(&music); pokeyOscInit(&pokeyOsc); sfxInit(&sfx); musSamples=0;
    musicPlay(&music);                 /* $26C0 */
    if(audio) SDL_PauseAudioDevice(audio,0);

    /* the instruction screen first, then the demo */
    demoStart();
    gstate=G_INTRO; stateTimer=T_INTRO;

    Uint32 last=SDL_GetTicks(); double acc=0; const double FT=1000.0/FPS;
    bool run=true;
    while(run){
        SDL_Event e;
        while(SDL_PollEvent(&e)){
            if(e.type==SDL_QUIT) run=false;
            if(e.type==SDL_KEYDOWN){
                SDL_Keycode kc=e.key.keysym.sym;
                if(kc==SDLK_ESCAPE) run=false;
                if(kc==SDLK_p) paused=!paused;
                /* the original's toggles: KBCODE $2D/$AD music on/off, $3E/$BE
                 * effects on/off -- unshifted enables, shifted disables */
                if(kc==SDLK_m){ musicOn=!musicOn; if(!musicOn) musicStop(&music); else musicPlay(&music); }
                if(kc==SDLK_n){ sfxOn=!sfxOn; }
                /* $3312 reads CONSOL: START gives one player, SELECT two.
                 * F1 and F2 stand in for those two console keys. */
                if(kc==SDLK_F1||kc==SDLK_F2||kc==SDLK_SPACE){
                    if(gstate!=G_MATCH_END){
                        /* $3312/$3337: START one player, SELECT two -- either way the
                         * demo ends here, the effects come on and the music goes off. */
                        int twoPlayer = (kc==SDLK_F2);
                        p1isCPU=0;
                        /* $2C90: a new match clears the level and both scores, then
                         * $2C9F bumps the starting skill and wraps it at 5 -- so the
                         * difficulty each game begins at rotates 1,2,3,4,1,... */
                        bg=0;boutCount=0;
                        level=0;
                        if(++aiSkill>=AI_SKILL_MAX) aiSkill=1;
                        fWins[0]=fWins[1]=0; fScore[0]=fScore[1]=0;
                        p2isCPU=!twoPlayer; p2.isCPU=p2isCPU;
                        /* $3337: both players get $50/$51, effects on, music off */
                        sfxOn=1; musicOn=0; musicStop(&music);
                        newBout();
                    }
                    else {
                        /* back to the attract state, which is a bout the game plays
                         * against itself: music on, effects off */
                        demoStart();
                    }
                }
            }
        }
        Uint32 now=SDL_GetTicks(); acc+=now-last; last=now;
        while(acc>=FT){
            acc-=FT;
            if(!paused){
                frameCount++;                 /* $394A: INC $14 every vertical blank */
                switch(gstate){
                    case G_FIGHT:
                        if(gameTickDue(0)) fightTick(SDL_GetKeyboardState(NULL));
                        break;
                    case G_POINT:
                        /* $3C01: the divider is longer while play is frozen, and
                         * $3017's loop drives one fighter, not both */
                        if(gameTickDue(1) && freezeWho)
                            fgtUpdate(freezeWho, freezeMove, rnd);
                        if(--stateTimer<=0){
                            freezeWho=NULL;
                            /* $2BA2: four points ends the bout */
                            if(fPoints[0]>=4){
                                fWins[0]++; signShow("WHITE",T_BEGIN); strcpy(banner,"");
                                gstate=G_ROUND_END; stateTimer=T_BEGIN;
                            } else if(fPoints[1]>=4){
                                fWins[1]++; boutLost=1;
                                signShow("RED",T_BEGIN); strcpy(banner,"");
                                gstate=G_ROUND_END; stateTimer=T_BEGIN;
                            }
                            else { resetPositions(); gstate=G_FIGHT; }
                        }
                        break;
                    case G_ROUND_END:
                        if(--stateTimer<=0){
                            /* Nothing is advanced here: the skill rises with the level
                             * once per bout ($2D09), the scene turns over on the bout
                             * count in newBout(), and the belt is read off the score.
                             *
                             * $2A26: the match lasts only while fighter 0 keeps winning.
                             * Lose a bout and $D0 becomes 6 -- the referee holds up MATCH
                             * OVER and the high-score table follows. */
                            if(boutLost){
                                gstate=G_MATCH_END; stateTimer=T_BEGIN;
                                signShow("MATCH OVER",T_BEGIN); strcpy(banner,"");
                            } else newBout();
                        }
                        break;
                    case G_MATCH_END:
                        if(--stateTimer<=0){
                            /* $5CC7..$5CEA: the finished score goes in as the candidate,
                             * and if it places, $5DE0 lets the loser put a name in the
                             * row it took. */
                            /* $5CBE: the loser's score, and the loser is fighter 0 */
                            int sc = fScore[0];
                            nameRow = hsSubmit(sc, hudBelt(sc), HS_START[0].name);
                            if(nameRow >= 0 && !p1isCPU){
                                hsNameBegin(&nameEntry, HS_START[0].name);
                                gstate=G_NAME;
                            } else {
                                nameRow=-1;
                                gstate=G_HISCORE; stateTimer=T_HISCORE;
                            }
                        }
                        break;
                    case G_NAME: {
                        /* the loser is always fighter 0 -- that is what ended the
                         * match ($2A26) -- so these are always player 1's keys */
                        Stick st = readKeys(SDL_GetKeyboardState(NULL), 0);
                        if(hsNameTick(&nameEntry, st.stick, st.fire)){
                            /* $5EA5: the three characters are read back into the entry */
                            /* $5EA5: the three characters go into the entry */
                            if(nameRow>=0)
                                memcpy(hsTable[nameRow].name,nameEntry.buf,HS_NAME_LEN);
                            nameRow=-1;
                            gstate=G_HISCORE; stateTimer=T_HISCORE;
                        }
                        break; }
                    case G_INTRO:
                    case G_HISCORE:
                        if(--stateTimer<=0) demoStart();
                        break;
                }
                vblank();
            }
        }

        drawBackground();
        if(gstate==G_INTRO || gstate==G_HISCORE || gstate==G_NAME){
            drawHiScore();
            /* the key bindings go on the ground too, in the game's own font, in the
             * rows above the table's header -- over the scene they were unreadable */
            /* The original needs no prompt -- the stick and the button are the only
             * controls it has. The port's mapping is its own, so it says so. */
            if(gstate==G_NAME){
                static const Col c={SHAPE_COL_GI_WHITE};
                groundTextC(1,"LEFT RIGHT PICK A LETTER",c);
                groundTextC(2,"FIRE TAKES IT",c);
            }
            if(gstate==G_INTRO){
                static const Col c={SHAPE_COL_GI_WHITE};
                groundTextC(0,"F1 ONE PLAYER    F2 TWO PLAYERS",c);
                groundTextC(1,"M MUSIC  N EFFECTS  P PAUSE  ESC QUIT",c);
                groundTextC(2,"P1 WASD LSHIFT   P2 ARROWS RSHIFT",c);
            }
        } else {
            drawReferee();
            drawSign();
            /* $3C2E composes the pair in an order set by $6114, the fighter an
             * ATTR_TURN frame last handed the turn to: $611C takes that fighter's
             * colour from $EA and $611D the other's. Ordering by x instead made the
             * two pop past each other the moment they crossed. */
            if(fgtTurnOwner){ drawFighter(&p1,COL_GI_P1); drawFighter(&p2,COL_GI_P2); }
            else            { drawFighter(&p2,COL_GI_P2); drawFighter(&p1,COL_GI_P1); }
            drawHUD();
        }
        if(gstate==G_POINT||gstate==G_ROUND_END||gstate==G_MATCH_END){
            drawTextC(100,banner,rgb(255,255,80));
            if(gstate==G_POINT) drawPopup();
        }
        if(paused) drawTextC(110,"PAUSED",rgb(255,255,255));

        SDL_RenderPresent(R);
    }
    if(audio) SDL_CloseAudioDevice(audio);
    SDL_DestroyRenderer(R); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}

