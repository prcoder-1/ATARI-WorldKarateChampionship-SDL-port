/* pokey.h - the game's music player ($1F06) and enough of POKEY to hear it.
 *
 * The player is ported statement for statement. A tick is one vertical blank, and
 * $1F06's divider skips one tick in twelve, so the music runs at 11/12 of the video
 * frame rate -- that is the tempo, and it is not a free parameter.
 *
 * Per voice, per tick:
 *   - if the note still has time left, only the envelope, vibrato and arpeggio run
 *   - otherwise read the next pattern event: a command (bit 7 set) configures the
 *     voice and is followed by more events; a note sets its length from MUS_DUR and
 *     its pitch, and resets the envelope
 *   - a pattern ends on $FF and steps the sequence; a sequence ends on $FF and loops
 *
 * The nine-byte shadow ($2168-$2170) is POKEY's register file:
 *   AUDF1 AUDC1 AUDF2 AUDC2 AUDF3 AUDC3 AUDF4 AUDC4 AUDCTL
 * AUDCTL is $50: channels 1+2 are joined into one 16-bit channel and channel 1 runs
 * at 1.79 MHz, which is voice 0; voices 1 and 2 are channels 3 and 4 at 64 kHz.
 */
#ifndef POKEY_H
#define POKEY_H

#include <stdint.h>
#include "generated/music.h"

#define POKEY_CLK_FAST 1789790.0   /* AUDCTL bit 6: channel 1 at the CPU clock */
#define POKEY_CLK_SLOW 63921.0     /* the default 64 kHz clock                 */

/* $2167 */
#define MUS_STOPPED 0x80
#define MUS_REQUEST 0x40

typedef struct {
    /* cleared on restart ($1F11 zeroes $214A..$2155) */
    int seqpos[3];      /* $214A */
    int dur[3];         /* $214D */
    int patpos[3];      /* $2150 */
    int note[3];        /* $2153 */
    /* persistent per-voice state */
    int instr[3];       /* $2156 envelope id     */
    int audcbase[3];    /* $2159 distortion bits */
    int vibdepth[3];    /* $215C */
    int arppos[3];      /* $2160 */
    int arpon[3];       /* $2163 */
    int viben[3];       /* $00CC */
    int envpos[3];      /* $00C9 */
    int basef[3];       /* $00C6 the note's AUDF, before vibrato/arpeggio */
    int tick;           /* $2149 */
    int divider;        /* $2166 */
    int state;          /* $2167 */
    int muted;          /* $00CF */
    uint8_t reg[9];     /* $2168..$2170 */
} Music;

static void musicInit(Music* m)
{
    int v;
    for (v = 0; v < 3; v++) {
        m->seqpos[v] = m->dur[v] = m->patpos[v] = m->note[v] = 0;
        m->instr[v] = MUS_INIT_INSTR[v];
        m->audcbase[v] = MUS_INIT_AUDC[v];
        m->vibdepth[v] = MUS_INIT_VIBDEPTH[v];
        m->arppos[v] = MUS_INIT_ARPPOS[v];
        m->arpon[v] = MUS_INIT_ARPON[v];
        m->viben[v] = 0;
        m->envpos[v] = 0;
        m->basef[v] = 0;
    }
    m->tick = 0;
    m->divider = MUS_DIVIDER_RELOAD;
    m->state = 0;
    m->muted = 0;
    for (v = 0; v < 9; v++) m->reg[v] = 0;
    m->reg[8] = MUS_AUDCTL;
}

static void musicPlay(Music* m) { m->state = MUS_REQUEST; }          /* $26C0 */
static void musicStop(Music* m) { m->state = MUS_REQUEST | MUS_STOPPED; } /* $26CF */

/* index of a voice's AUDF byte in the shadow: $C5 = (voice+1)*2 */
static int musC5(int v) { return (v + 1) * 2; }

/* $1F5A..$1FDA: advance to the next note, running any commands on the way */
static void musicNextEvent(Music* m, int v)
{
    int guard;
    for (guard = 0; guard < 64; guard++) {
        int b = MUS_SEQ[v][m->seqpos[v]];
        if (b == 0xFF) {                       /* $1F6D: sequence loops */
            m->dur[v] = 0;
            m->seqpos[v] = 0;
            m->patpos[v] = 0;
            continue;
        }
        if (b == 0xFE) { musicStop(m); return; }
        if (b >= MUS_PATTERNS) { m->seqpos[v] = 0; continue; }
        {
            const uint8_t* pat = MUS_PAT[b];
            int y = m->patpos[v], e, c5 = musC5(v);
            for (;;) {                         /* $1F89: commands, then a note */
                e = pat[y];
                if (!(e & 0x80)) break;
                switch (e & 0x0F) {            /* $20C2 jump table */
                    case 0: y++; m->instr[v] = pat[y] % MUS_ENVELOPES; break;
                    case 1: y++; m->vibdepth[v] = pat[y]; m->viben[v] = 1; break;
                    case 2: m->viben[v] = 0; break;
                    case 3: m->audcbase[v] = 0x80; break;
                    case 4: m->audcbase[v] = 0xA0; break;
                    case 5: m->arpon[v] = 1; break;
                    case 6: m->arpon[v] = 0; break;
                    case 7: m->audcbase[v] = 0xC0; break;
                }
                y++;
            }
            m->dur[v] = MUS_DUR[(e & 0x1F) % 9];
            y++;
            m->patpos[v] = y;
            m->note[v] = pat[y];               /* $1FA5 */
            if (v == 0) {                      /* channels 1+2, 16-bit */
                uint16_t f = MUS_NOTE16[m->note[v] % 32];
                m->reg[0] = (uint8_t)(f & 0xFF);
                m->reg[2] = (uint8_t)(f >> 8);
            } else {
                m->basef[v] = MUS_NOTE8[m->note[v] % 46];
                m->reg[c5] = (uint8_t)m->basef[v];
            }
            m->envpos[v] = 0;                  /* $1FDA */
            y++;
            m->patpos[v] = y;
            if (pat[y] == 0xFF) {              /* pattern ended */
                m->patpos[v] = 0;
                m->seqpos[v]++;
            }
            return;
        }
    }
}

/* $1FF2..$2079: envelope, vibrato, arpeggio, then count the note down */
static void musicVoiceTail(Music* m, int v)
{
    int c5 = musC5(v);
    const uint8_t* env = MUS_ENV[m->instr[v] % MUS_ENVELOPES];
    int val = env[m->envpos[v]];

    if (val == 0xFF) {
        /* envelope finished: hold whatever is sounding */
    } else if (val & 0x80) {                   /* $200F: release */
        m->reg[c5] = 0;
        m->reg[c5 + 1] = (uint8_t)val;
        m->envpos[v]++;
        m->dur[v]--;
        return;                                /* $2079 via the release path */
    } else {                                   /* $2020 */
        m->envpos[v]++;
        m->reg[c5 + 1] = (uint8_t)(val | m->audcbase[v]);
        if (v != 0) m->reg[c5] = (uint8_t)m->basef[v];
    }

    if (m->viben[v]) {                         /* $2033 */
        int d = (m->tick & 1) ? 0 : m->vibdepth[v];
        int n = (m->note[v] - d) & 0xFF;
        m->reg[c5] = MUS_NOTE8[n % 46];
    }
    if (m->arpon[v]) {                         /* $2058 */
        int guard;
        for (guard = 0; guard < 16; guard++) {
            int a = MUS_ARP[m->arppos[v] % 9];
            m->arppos[v] = (m->arppos[v] + 1) % 9;
            if (a != 0x1F) {
                m->reg[c5] = (uint8_t)((a + m->basef[v]) & 0xFF);
                break;
            }
            m->arppos[v] = 0;
        }
    }
    m->dur[v]--;
}

/* $1F06: one vertical blank */
static void musicTick(Music* m)
{
    int v;
    if (--m->divider < 0) {                    /* skips one tick in twelve */
        m->divider = MUS_DIVIDER_RELOAD;
        return;
    }
    if (m->state & MUS_STOPPED) {              /* $1F28 */
        if (m->state & MUS_REQUEST) {
            for (v = 0; v < 8; v++) m->reg[v] = 0;
            m->state = MUS_STOPPED;
        }
        return;
    }
    if (m->state & MUS_REQUEST) {              /* $1F11: restart */
        for (v = 0; v < 3; v++)
            m->seqpos[v] = m->dur[v] = m->patpos[v] = m->note[v] = 0;
        m->state = 0;
    }
    m->tick++;                                 /* $2149, the vibrato phase */
    for (v = 2; v >= 0; v--) {
        if (m->dur[v] == 0) musicNextEvent(m, v);
        musicVoiceTail(m, v);
    }
}

/* ---------------- POKEY tone generation ----------------
 * Each channel is a square wave (or a poly-noise sequence) at
 *   f = clock / (2 * (AUDF + 1))
 * gated by the volume nibble of AUDC. AUDC bits 7..5 select the distortion:
 * 5 and 7 are a pure tone, 2 and 6 the 4-bit poly, the rest noise. Bit 4 is
 * volume-only output. */
typedef struct {
    double phase[3];
    uint32_t lfsr17, lfsr4;
    int level[3];
} PokeyOsc;

static void pokeyOscInit(PokeyOsc* o)
{
    int i;
    for (i = 0; i < 3; i++) { o->phase[i] = 0.0; o->level[i] = 1; }
    o->lfsr17 = 1; o->lfsr4 = 1;
}

static int pokeyNextBit(PokeyOsc* o, int dist)
{
    if (dist == 2 || dist == 6) {              /* 4-bit poly */
        int b = ((o->lfsr4 >> 3) ^ (o->lfsr4 >> 2)) & 1;
        o->lfsr4 = ((o->lfsr4 << 1) | b) & 0x0F;
        return (int)(o->lfsr4 & 1);
    }
    {                                          /* 17-bit poly, used as noise */
        int b = ((o->lfsr17 >> 16) ^ (o->lfsr17 >> 11)) & 1;
        o->lfsr17 = ((o->lfsr17 << 1) | b) & 0x1FFFF;
        return (int)(o->lfsr17 & 1);
    }
}

/* Render n samples of the three voices into out (added, not replaced). */
static void pokeyRender(PokeyOsc* o, const Music* m, int16_t* out, int n, int rate)
{
    int v, i;
    for (v = 0; v < 3; v++) {
        int c5 = musC5(v);
        int audc = m->reg[c5 + 1];
        int vol = audc & 0x0F;
        int dist = (audc >> 5) & 7;
        int volonly = audc & 0x10;
        double freq;
        if (!vol) continue;
        if (v == 0) {
            int div = m->reg[0] | (m->reg[2] << 8);
            freq = POKEY_CLK_FAST / (2.0 * (div + 1));
        } else {
            freq = POKEY_CLK_SLOW / (2.0 * (m->reg[c5] + 1));
        }
        if (freq < 20.0 || freq > rate / 2.0) continue;
        if (volonly) {
            for (i = 0; i < n; i++) out[i] = (int16_t)(out[i] + vol * 90);
            continue;
        }
        {
            double step = freq * 2.0 / rate;   /* toggles per sample */
            int pure = (dist == 5 || dist == 7);
            for (i = 0; i < n; i++) {
                o->phase[v] += step;
                while (o->phase[v] >= 1.0) {
                    o->phase[v] -= 1.0;
                    o->level[v] = pure ? !o->level[v] : pokeyNextBit(o, dist);
                }
                out[i] = (int16_t)(out[i] + (o->level[v] ? vol * 100 : -vol * 100));
            }
        }
    }
}

#endif
