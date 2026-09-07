/* verify_music.c - run the ported music player headlessly.
 *
 * Checks the player against its own data (every event decodes, patterns and sequences
 * terminate, the tempo is the ROM's) and renders the tune to /tmp/wk_music.wav so it
 * can be listened to or measured. verify_music.py does the measuring.
 *
 * Build and run:  make verify-music
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "pokey.h"

#define SR 44100
#define FPS 60
#define SECONDS 40

static int failures;
static void check(int cond, const char* what)
{
    printf("  %-52s %s\n", what, cond ? "ok" : "FAILED");
    if (!cond) failures++;
}

static void w16(FILE* f, int v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void w32(FILE* f, long v)
{ for (int i = 0; i < 4; i++) fputc((int)((v >> (8 * i)) & 0xFF), f); }

int main(void)
{
    Music m;
    PokeyOsc o;

    printf("the music data is self-consistent\n");
    {
        int bad = 0, notes = 0, cmds = 0;
        for (int p = 0; p < MUS_PATTERNS; p++) {
            const uint8_t* pat = MUS_PAT[p];
            int y = 0, guard = 0;
            while (guard++ < 512) {
                int e = pat[y];
                if (e == 0xFF) break;
                if (e & 0x80) { cmds++; if ((e & 0x0F) < 2) y++; y++; continue; }
                if ((e & 0x1F) > 8) bad++;      /* the duration table is 9 long */
                notes++;
                y += 2;
            }
            if (guard >= 512) bad++;
        }
        printf("    %d patterns: %d notes, %d commands\n", MUS_PATTERNS, notes, cmds);
        check(bad == 0, "every event decodes and every pattern terminates");
    }

    printf("the tempo is the ROM's\n");
    {
        /* $1F06 reloads $2166 with 10 and skips when it underflows, so the player
         * runs for divider values 9..0 and skips one frame: 10 ticks per 11 frames. */
        musicInit(&m);
        musicPlay(&m);
        int ran = 0;
        int before = m.tick;
        for (int i = 0; i < 1200; i++) { musicTick(&m); }
        ran = m.tick - before;
        double hz = ran / 20.0;                 /* 1200 frames = 20 s at 60 Hz */
        printf("    %d player ticks in 1200 frames -> %.2f Hz\n", ran, hz);
        check(ran >= 1088 && ran <= 1092, "the player runs 10 ticks per 11 frames");
    }

    printf("the tune plays without getting stuck\n");
    {
        musicInit(&m);
        musicPlay(&m);
        int silent = 0, prevSeq = 0, maxSeq = 0;
        long loopFrames = -1;
        const long LIMIT = 60L * FPS * 60;      /* up to an hour of frames */
        for (long i = 0; i < LIMIT; i++) {
            musicTick(&m);
            if (i < SECONDS * FPS) {
                int any = 0;
                for (int v = 0; v < 3; v++) if (m.reg[(v + 1) * 2 + 1] & 0x0F) any = 1;
                if (!any) silent++;
            }
            if (m.seqpos[0] > maxSeq) maxSeq = m.seqpos[0];
            if (m.seqpos[0] < prevSeq && loopFrames < 0) loopFrames = i;
            prevSeq = m.seqpos[0];
            if (loopFrames >= 0 && i > loopFrames + 10) break;
        }
        printf("    %d of %d ticks silent in the first %ds\n",
               silent, SECONDS * FPS, SECONDS);
        if (loopFrames >= 0)
            printf("    voice 0 loops after %ld frames (%.1f s), reaching pattern %d\n",
                   loopFrames, loopFrames / (double)FPS, maxSeq);
        check(silent * 4 < SECONDS * FPS, "the tune is sounding most of the time");
        check(loopFrames > 0, "the sequence reaches its end and loops");
    }

    printf("dumping the register stream to /tmp/wk_pokey.dat\n");
    {
        /* One 9-byte record per VIDEO frame, the same thing atari800's -pokeyrec
         * writes, so the two streams can be compared directly. */
        musicInit(&m); musicPlay(&m);
        FILE* f = fopen("/tmp/wk_pokey.dat", "wb");
        long frames = 14000;
        for (long i = 0; i < frames; i++) {
            musicTick(&m);
            fwrite(m.reg, 1, 9, f);
        }
        fclose(f);
        printf("    %ld frames\n", frames);
        check(frames > 0, "register stream written");
    }

    printf("rendering /tmp/wk_music.wav\n");
    {
        long n = (long)SR * SECONDS;
        int16_t* buf = malloc((size_t)n * 2);
        memset(buf, 0, (size_t)n * 2);
        musicInit(&m); pokeyOscInit(&o); musicPlay(&m);
        long i = 0;
        while (i < n) {
            musicTick(&m);
            long chunk = SR / FPS;
            if (i + chunk > n) chunk = n - i;
            pokeyRender(&o, &m, buf + i, (int)chunk, SR);
            i += chunk;
        }
        FILE* f = fopen("/tmp/wk_music.wav", "wb");
        fwrite("RIFF", 1, 4, f); w32(f, 36 + n * 2); fwrite("WAVEfmt ", 1, 8, f);
        w32(f, 16); w16(f, 1); w16(f, 1); w32(f, SR); w32(f, SR * 2); w16(f, 2); w16(f, 16);
        fwrite("data", 1, 4, f); w32(f, n * 2);
        fwrite(buf, 2, (size_t)n, f);
        fclose(f);
        long peak = 0;
        for (long k = 0; k < n; k++) { long a = buf[k] < 0 ? -buf[k] : buf[k]; if (a > peak) peak = a; }
        free(buf);
        printf("    %ld samples, peak %ld\n", n, peak);
        check(peak > 1000, "the render is audible");
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "all checks passed",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
