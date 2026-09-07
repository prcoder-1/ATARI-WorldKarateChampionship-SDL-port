/* verify_sfx.c - exercise the ported sound effects headlessly.
 *
 * Checks the six digitised effects and the logic that fires them, and renders each to
 * /tmp/wk_sfx<N>.wav so they can be listened to.
 *
 * Build and run:  make verify-sfx
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "sfx.h"

#define SR 44100
static int failures;
static void check(int cond, const char* what)
{
    printf("  %-56s %s\n", what, cond ? "ok" : "FAILED");
    if (!cond) failures++;
}

static void w16(FILE* f, int v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void w32(FILE* f, long v)
{ for (int i = 0; i < 4; i++) fputc((int)((v >> (8 * i)) & 0xFF), f); }

int main(void)
{
    Sfx s;

    printf("the effect table is sane\n");
    {
        int bad = 0;
        for (int i = 0; i < SFX_COUNT; i++) {
            if (SFX_START[i] >= SFX_END[i]) bad++;
            if (SFX_END[i] > sizeof SFX_DATA) bad++;
        }
        printf("    %d effects, %d bytes of sample data, %.0f Hz\n",
               SFX_COUNT, (int)sizeof SFX_DATA, (double)SFX_RATE);
        check(bad == 0, "every effect's range lies inside the sample data");
    }

    printf("each effect plays to its end\n");
    {
        int bad = 0;
        for (int i = 0; i < SFX_COUNT; i++) {
            sfxInit(&s);
            if (!sfxStart(&s, i, 1)) { bad++; continue; }
            long steps = 0;
            while (sfxStep(&s) && steps < 20000) steps++;
            long want = SFX_END[i] - SFX_START[i];
            if (steps != want - 1 && steps != want) bad++;
            printf("    effect %d: %5ld samples, %.2f s, %s nibble\n",
                   i, steps, steps / (double)SFX_RATE,
                   SFX_HIGH_NIBBLE[i] ? "high" : "low");
        }
        check(bad == 0, "every effect runs its full length and stops");
    }

    printf("the enable flag gates them, as $0055 does\n");
    {
        sfxInit(&s);
        check(!sfxStart(&s, 0, 0), "with effects disabled, nothing starts");
        check(sfxStart(&s, 0, 1), "with them enabled, it starts");
        check(!sfxStart(&s, SFX_COUNT, 1), "an out-of-range number is ignored ($3A7C)");
    }

    printf("the trigger tables map to real effects\n");
    {
        int bad = 0, reachable[SFX_COUNT];
        memset(reachable, 0, sizeof reachable);
        for (int i = 0; i < SFX_EVENTS; i++) {
            const uint8_t* t[3] = { SFX_WHILE_FIGHTING, SFX_WHILE_FROZEN, SFX_STATE_2_5 };
            for (int k = 0; k < 3; k++) {
                int v = (int8_t)t[k][i];
                if (v < 0) continue;
                if (v >= SFX_COUNT) bad++; else reachable[v] = 1;
            }
            int dv = (int8_t)SFX_DELAYED[i];
            if (SFX_DELAY[i] && (dv < 0 || dv >= SFX_COUNT)) bad++;
        }
        int n = 0;
        for (int i = 0; i < SFX_COUNT; i++) n += reachable[i];
        printf("    %d of %d effects are reachable from the tables\n", n, SFX_COUNT);
        check(bad == 0, "no table entry names an effect that does not exist");
        check(n == SFX_COUNT, "every effect is reachable");
    }

    printf("a shape change fires a sound, and only for the listed shapes\n");
    {
        int fired = 0, quiet = 0;
        for (int shape = 0; shape < 54; shape++) {
            sfxInit(&s);
            int e = sfxForShapeChange(&s, shape, 0xFF, 1, 0);
            int listed = 0;
            for (int i = 0; i < SFX_EVENTS; i++) if (SFX_EVENT_SHAPE[i] == shape) listed = 1;
            if (e >= 0) fired++;
            else if (!listed) quiet++;
        }
        printf("    %d shapes fire while fighting; %d unlisted shapes stay quiet\n",
               fired, quiet);
        check(fired > 0, "listed shapes fire");
        check(quiet == 54 - SFX_EVENTS, "unlisted shapes never fire");

        /* $3A2E: while fighting and not frozen, the frame's attributes must pass */
        sfxInit(&s);
        int withAttr = sfxForShapeChange(&s, SFX_EVENT_SHAPE[0], SFX_ATTR_MASK, 1, 0);
        sfxInit(&s);
        int noAttr = sfxForShapeChange(&s, SFX_EVENT_SHAPE[0], 0, 1, 0);
        check(withAttr >= 0 && noAttr < 0,
              "the attribute mask $4D gates the fighting-state sounds");

        /* the referee's announcements come from the frozen table */
        sfxInit(&s);
        int frozen = sfxForShapeChange(&s, SFX_EVENT_SHAPE[8], 0, 1, 1);
        check(frozen >= 0, "a frozen-state shape change announces");
    }

    printf("a delayed second effect is armed and fires ($617C/$617D)\n");
    {
        int idx = -1;
        for (int i = 0; i < SFX_EVENTS; i++) if (SFX_DELAY[i]) { idx = i; break; }
        sfxInit(&s);
        sfxForShapeChange(&s, SFX_EVENT_SHAPE[idx], 0, 1, 1);
        printf("    shape %d arms effect %d after %d frames\n",
               SFX_EVENT_SHAPE[idx], SFX_DELAYED[idx], SFX_DELAY[idx]);
        check(s.delay == SFX_DELAY[idx], "the delay is armed");
        int shapes[2] = { 0, 0 }, attrs[2] = { 0, 0 };
        for (int f = 0; f < SFX_DELAY[idx]; f++) sfxFrame(&s, shapes, attrs, 1, 1, 1);
        check(sfxActive(&s), "and the delayed effect starts when it expires");
    }

    printf("rendering the effects\n");
    {
        int quiet = 0;
        for (int i = 0; i < SFX_COUNT; i++) {
            long n = (long)((SFX_END[i] - SFX_START[i]) / (double)SFX_RATE * SR) + SR / 10;
            int16_t* buf = calloc((size_t)n, 2);
            sfxInit(&s); sfxStart(&s, i, 1);
            sfxRender(&s, buf, (int)n, SR);
            long peak = 0, nz = 0;
            for (long k = 0; k < n; k++) {
                long a = buf[k] < 0 ? -buf[k] : buf[k];
                if (a > peak) peak = a;
                if (buf[k]) nz++;
            }
            char path[64];
            snprintf(path, sizeof path, "/tmp/wk_sfx%d.wav", i);
            FILE* f = fopen(path, "wb");
            fwrite("RIFF", 1, 4, f); w32(f, 36 + n * 2); fwrite("WAVEfmt ", 1, 8, f);
            w32(f, 16); w16(f, 1); w16(f, 1); w32(f, SR); w32(f, SR * 2); w16(f, 2); w16(f, 16);
            fwrite("data", 1, 4, f); w32(f, n * 2);
            fwrite(buf, 2, (size_t)n, f); fclose(f);
            printf("    effect %d -> %s  peak %ld, %ld%% non-silent\n",
                   i, path, peak, 100 * nz / n);
            if (peak < 500) quiet++;
            free(buf);
        }
        check(quiet == 0, "every effect renders audible audio");
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILURES" : "all checks passed",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
