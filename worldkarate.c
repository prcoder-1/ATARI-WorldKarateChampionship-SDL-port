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
enum { G_TITLE, G_FIGHT, G_POINT, G_ROUND_END, G_MATCH_END };
static int gstate=G_TITLE;
static int stateTimer=0;
static Fighter p1,p2;
static int roundTime;      /* $6154: referee traversals left in the round */
static int roundClock;     /* $00DC: the BCD seconds the HUD shows */
static int tickCounter;    /* $6121: video frames since the last game tick */
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
/* $6159/$615B/$615A/$615C/$615E: the referee's current action -- how far through it he
 * is, which way it counts, its step, which action, and whether one is running. Finishing
 * an action is what ticks the round counter down. */
static int refProgress, refDir, refStep, refBusy;
static void refereeBegin(void);
static int roundIndex;     /* $615F */
static int refTurns;       /* $6162: completed referee actions */
static int bg=0;
static int dan=0;
static char banner[64]="";
/* $615C picks one of the referee's three actions; what the player actually reads is the
 * sign he holds up beside him. The bitmaps are captured (generated/signs.h) and named
 * there; the port asks for one by name so a missing sign fails visibly rather than
 * silently drawing the wrong board. */
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

/* Sprite overlap, which is what the hardware's player-player collision reports.
 * Replaces the old invented limb-hitbox test. */
static int spritesOverlap(const Fighter* a, const Fighter* b)
{
    const ShapePM* sa=&SHAPE_PM[a->shape % SHAPE_COUNT];
    const ShapePM* sb=&SHAPE_PM[b->shape % SHAPE_COUNT];
    if(!sa->h||!sb->h||!sa->px||!sb->px) return 0;
    int ax=spriteLeft(sa,a), bx=spriteLeft(sb,b);
    for(int y=0;y<sa->h;y++){
        int sy=sa->y0+y-sb->y0;
        if(sy<0||sy>=sb->h) continue;
        for(int k=0;k<sa->w;k++){
            if(!sa->px[y*sa->w+k]) continue;
            int ca = a->facing ? (sa->w-1-k) : k;
            int px = ax+ca*FX_SCALE;
            int kb = (px-bx)/FX_SCALE;
            if(kb<0||kb>=sb->w) continue;
            int cb = b->facing ? (sb->w-1-kb) : kb;
            if(sb->px[sy*sb->w+cb]) return 1;
        }
    }
    return 0;
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

/* ---------------- HUD ---------------- */
static int lastBonus=0;
static uint8_t hudCells[HUD_CELLS];

static void drawHUD(void)
{
    int demo = !(p1.isHuman || p2.isHuman);
    hudCompose(hudCells, p1.isHuman, p2.isHuman, demo, roundClock,
               ((dan+1)/10)*16 + ((dan+1)%10), dan, p1.score, p2.score,
               p1.wins, p2.wins, gstate==G_TITLE);
    hudDraw(hudCells, LW, LH, setpx1);
    hudMarkers(p1.points, p2.points, LW, LH, setpx1);
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
    int cpu=p2.isCPU, w1=p1.wins, w2=p2.wins, s1=p1.points, s2=p2.points;
    /* $2F7C places both fighters at $54 */
    placeFighter(&p1,T_START_X-0x20,0,0);   /* faces right */
    placeFighter(&p2,T_START_X+0x20,1,1);   /* faces left  */
    p2.isCPU=cpu; p1.wins=w1; p2.wins=w2; p1.points=s1; p2.points=s2;
    /* $0050,y: nonzero = joystick, zero = the CPU routine */
    p1.isHuman=1;
    p2.isHuman=!p2.isCPU;
}

static void newBout(void)
{
    p1.points=0; p2.points=0;
    /* $2F5F: the round length is a count of referee traversals, not seconds */
    roundTime=ROUND_TRAVERSALS[roundIndex%3];
    /* $2D25/$2D3D: 30 seconds against the computer, 60 with two players */
    roundClock = p2.isCPU ? T_CLOCK_1P : T_CLOCK_2P;
    clockTick = T_CLOCK_TICK;
    tickCounter = 0;
    refBusy=0; refTurns=0; refereeBegin();
    signShow("BEGIN",T_BEGIN);
    resetPositions();
    gstate=G_FIGHT;
   }

/* $58EF: begin one of the referee's three signalling actions. Which one comes from
 * $58A1, by round number and a random draw; its speed from $58AD; which way its progress
 * counter runs from $5885. He does not move: this is a timer, not a walk. */
static void refereeBegin(void)
{
    int i = (roundIndex*4 + (int)(rnd()&3)) % 12;
    /* REF_ACTION[i] picks which of his three signals this is; they differ in the
     * markers and the sign, not in his figure, of which only one pose was captured. */
    refStep   = REF_STEP[i];
    refDir    = REF_SIDE[refTurns % 12] & 1;
    refProgress = refDir ? REF_START_DOWN : REF_START_UP;
    refBusy = 1;
}

/* $5807: advance the current action. Returns 1 when one completes, which is what $5834
 * uses to take one off the round counter. */
static int refereeStep(void)
{
    if(!refBusy){ refereeBegin(); return 0; }
    if(refDir){
        refProgress -= refStep;
        if(refProgress >= REF_END_LOW) return 0;
    } else {
        refProgress += refStep;
        if(refProgress < REF_END_HIGH) return 0;
    }
    refBusy = 0;
    refTurns++;
    return 1;
}

/* $2FFA/$3004: a scoring blow freezes play and forces the loser into move 17 or 18,
 * chosen by whether the blow came from the front or from behind. */
static int lastBonusOf(int val){ return val>=2 ? 200 : 100; }

static void award(Fighter* a,int val)
{
    Fighter* d=(a==&p1)?&p2:&p1;
    a->points+=val;
    a->score += lastBonusOf(val);
    int total=ROUND_TRAVERSALS[roundIndex%3];
    int elapsed=(total-roundTime)*0x28/(total?total:1);
    lastBonus=score_time_bonus(elapsed)*100;
    /* the game's own wording, off its signs: a half point or a full one */
    signShow(val>=2 ? "FULL POINT" : "HALF POINT", T_FREEZE);
    strcpy(banner, "");
       gstate=G_POINT; stateTimer=T_FREEZE;   /* $2FEA: 128 frames */
    int fromFront = (a->x < d->x) ? (d->facing==1) : (d->facing==0);
    d->queued = fromFront ? MOVE_HIT : MOVE_FALL;
    d->samemove=0;
    fgtStartMove(d,d->queued,rnd);            /* $3011/$2747 */
    /* $2FFE: $D1 is the struck fighter, and $6131 is 0 here ($289D/$2D9F) */
    freezeWho = d;
    freezeMove = 0;
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
    if(am_is_attack_move(p1.move) && spritesOverlap(&p1,&p2)){
        award(&p1, p1.move>=11 ? 2 : 1); return;
    }
    if(am_is_attack_move(p2.move) && spritesOverlap(&p2,&p1)){
        award(&p2, p2.move>=11 ? 2 : 1); return;
    }
}

/* one video frame, whatever the game state: everything the original does from its
 * vertical blank -- the clock, the referee, the sound watcher */
static void vblank(void)
{
    if(gstate==G_FIGHT){
        /* $3988: a BCD decrement once a second */
        if(--clockTick<=0){
            clockTick=T_CLOCK_TICK;
            if(roundClock){
                int lo=roundClock&0x0F, hi=roundClock>>4;
                if(lo) lo--; else { lo=9; if(hi) hi--; }
                roundClock=(hi<<4)|lo;
            }
        }
        if((refereeStep() && --roundTime<=0) || roundClock==0){
            gstate=G_ROUND_END; stateTimer=T_BEGIN;
            /* the signs name the fighters by their gi, RED and WHITE */
            if(p1.points>p2.points){ p1.wins++; signShow("WHITE",T_BEGIN); strcpy(banner,""); }
            else if(p2.points>p1.points){ p2.wins++; signShow("RED",T_BEGIN); strcpy(banner,""); }
            else { signIndex=-1; strcpy(banner,"DRAW"); }
        }
        if(p1.points>=4){ p1.wins++; signShow("WHITE",T_BEGIN); strcpy(banner,""); gstate=G_ROUND_END; stateTimer=T_BEGIN; }
        if(p2.points>=4){ p2.wins++; signShow("RED",T_BEGIN); strcpy(banner,""); gstate=G_ROUND_END; stateTimer=T_BEGIN; }
    }
    if(signTimer>0 && --signTimer==0) signIndex=-1;
    soundFrame();
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
    musicInit(&music); pokeyOscInit(&pokeyOsc); sfxInit(&sfx); musSamples=0;
    musicPlay(&music);                 /* $26C0 */
    if(audio) SDL_PauseAudioDevice(audio,0);

    resetPositions(); p2.isCPU=p2isCPU;

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
                    if(gstate==G_TITLE){
                        int twoPlayer = (kc==SDLK_F2);
                        dan=0;bg=0;aiSkill=1;roundIndex=0;
                        p1.wins=p2.wins=0; p1.score=p2.score=0;
                        p2isCPU=!twoPlayer; p2.isCPU=p2isCPU;
                        /* $3337: both players get $50/$51, effects on, music off */
                        sfxOn=1; musicOn=0; musicStop(&music);
                        newBout();
                    }
                    else if(gstate==G_MATCH_END){
                        gstate=G_TITLE;
                        /* back to the attract state: music on, effects off */
                        musicOn=1; sfxOn=0; musicPlay(&music);
                    }
                }
            }
        }
        Uint32 now=SDL_GetTicks(); acc+=now-last; last=now;
        while(acc>=FT){
            acc-=FT;
            if(!paused){
                switch(gstate){
                    case G_TITLE: break;
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
                            if(p1.points>=4||p2.points>=4){ gstate=G_ROUND_END; stateTimer=T_BEGIN; }
                            else { resetPositions(); gstate=G_FIGHT; }
                        }
                        break;
                    case G_ROUND_END:
                        if(--stateTimer<=0){
                            if(p1.wins>=1 && (p1.points>=4 || roundTime<=0)){
                                bg=(bg+1)%NSCENES; dan++;
                                /* $2C99: the skill level rises and wraps back to 1 */
                                if(++aiSkill>=5) aiSkill=1;
                                if(dan>=NSCENES){ gstate=G_MATCH_END; strcpy(banner,"BLACK BELT!"); }
                                else newBout();
                            } else newBout();
                        }
                        break;
                    case G_MATCH_END: break;
                }
                vblank();
            }
        }

        drawBackground();
        if(gstate!=G_TITLE){
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
        if(gstate==G_TITLE){
            drawTextC(70,"WORLD KARATE",rgb(255,255,255));
            drawTextC(84,"CHAMPIONSHIP",rgb(255,255,255));
            drawTextC(112,"SDL PORT",rgb(255,200,60));
            drawTextC(140,"F1 START ONE PLAYER",rgb(200,200,200));
            drawTextC(154,"F2 SELECT TWO PLAYERS",rgb(200,200,200));
            drawTextC(176,"P1 WASD LSHIFT   P2 ARROWS RSHIFT",rgb(120,140,180));
            drawTextC(190,"M MUSIC   N EFFECTS   P PAUSE   ESC QUIT",rgb(120,140,180));
        } else if(gstate==G_POINT||gstate==G_ROUND_END||gstate==G_MATCH_END){
            drawTextC(100,banner,rgb(255,255,80));
            if(gstate==G_POINT && lastBonus>0){ char bb[24]; sprintf(bb,"BONUS %d",lastBonus);
                drawTextC(116,bb,rgb(255,200,80)); }
            if(gstate==G_MATCH_END) drawTextC(124,"PRESS SPACE",rgb(200,200,200));
        }
        if(paused) drawTextC(110,"PAUSED",rgb(255,255,255));

        SDL_RenderPresent(R);
    }
    if(audio) SDL_CloseAudioDevice(audio);
    SDL_DestroyRenderer(R); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}

