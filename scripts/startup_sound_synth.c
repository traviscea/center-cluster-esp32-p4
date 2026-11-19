// Builds main/sound/startup_sound_pcm.c: the startup sound (shimmer as the STI
// badge glows in, turbo spooling through the needle sweep, HKS SSQV-style
// blow-off as the face lights up), rendered ahead of time so the cluster
// doesn't spend boot time synthesizing it.
//
//   cc -O2 -o gen scripts/startup_sound_synth.c -lm && ./gen > main/sound/startup_sound_pcm.c
//
// Timings must match UI_LOGO_DELAY_MS / UI_SWEEP_* in tach_ui/screens/ui_Screen1.h.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#define UI_LOGO_DELAY_MS   300
#define UI_SWEEP_DELAY_MS  1200
#define UI_SWEEP_UP_MS     800
#define UI_SWEEP_HOLD_MS   250
#define UI_SWEEP_DOWN_MS   900
#define SOUND_STARTUP_GAIN 1.0f          // level is applied on the cluster (sound.h)
#define SR                 32000
#define TWO_PI             6.28318530718f
#define STARTUP_SEC        4.4f
#define MALLOC_CAP_SPIRAM  0
#define heap_caps_calloc(n, s, c) calloc(n, s)
#define heap_caps_malloc(n, c)    malloc(n)
#define ESP_LOGE(tag, ...)        fprintf(stderr, __VA_ARGS__)
static const char *TAG = "gen";
static int16_t *s_startup = NULL;
static int s_startup_len = 0;

// ---------------------------------------------------------------- startup clip
// Rendered once at boot into a buffer, then played from the point matching the
// screen's startup clock, so it stays locked to the badge and the sweep.

static uint32_t rng_state = 0x12345678;
static float noise(void)              // roughly Gaussian white noise
{
    float s = 0;
    for (int i = 0; i < 4; i++) {
        rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
        s += (float)(rng_state & 0xFFFF) / 65535.0f - 0.5f;
    }
    return s * 1.73f;
}
static float smooth(float k) { if (k < 0) k = 0; if (k > 1) k = 1; return k * k * (3 - 2 * k); }
static float lp_step(float *st, float x, float fc) { float a = expf(-TWO_PI * fc / SR); *st = a * *st + (1 - a) * x; return *st; }

typedef struct { float x1, x2, y1, y2; } bq_t;
static float bp_step(bq_t *s, float x, float fc, float q)     // resonant bandpass
{
    float w = TWO_PI * fc / SR, al = sinf(w) / (2 * q), c = cosf(w), a0 = 1 + al;
    float y = (al / a0) * x - (al / a0) * s->x2 + (2 * c / a0) * s->y1 - ((1 - al) / a0) * s->y2;
    s->x2 = s->x1; s->x1 = x; s->y2 = s->y1; s->y1 = y;
    return y;
}
static void normalize(float *b, int n, float peak)
{
    float m = 0;
    for (int i = 0; i < n; i++) m = fmaxf(m, fabsf(b[i]));
    if (m > 0) for (int i = 0; i < n; i++) b[i] *= peak / m;
}

static bool render_startup(void)
{
    int N = (int)(STARTUP_SEC * SR);
    float *mix = heap_caps_calloc(N, sizeof(float), MALLOC_CAP_SPIRAM);
    float *a = heap_caps_malloc(SR * 2 * sizeof(float), MALLOC_CAP_SPIRAM);   // scratch, 2 s
    float *b = heap_caps_malloc(SR * 2 * sizeof(float), MALLOC_CAP_SPIRAM);
    float *c = heap_caps_malloc(SR * 2 * sizeof(float), MALLOC_CAP_SPIRAM);
    s_startup = heap_caps_malloc(N * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!mix || !a || !b || !c || !s_startup) {
        ESP_LOGE(TAG, "no memory for the startup sound");
        free(mix); free(a); free(b); free(c); free(s_startup); s_startup = NULL;
        return false;
    }

    // 1) STI badge: soft shimmering chord that swells with the glow, then fades
    {
        int start = (int)(UI_LOGO_DELAY_MS / 1000.0f * SR), n = (int)(1.6f * SR);
        for (int i = 0; i < n && start + i < N; i++) {
            float t = (float)i / SR;
            float swell = (t < 0.6f ? smooth(t / 0.6f) : 1.0f) * expf(-fmaxf(0, t - 0.6f) / 0.45f);
            float vib = 0.3f * sinf(TWO_PI * 5 * t);
            float chord = 0.5f * sinf(TWO_PI * 440.0f * t + vib) + 0.35f * sinf(TWO_PI * 659.25f * t + vib)
                        + 0.3f * sinf(TWO_PI * 880.0f * t + vib) + 0.12f * sinf(TWO_PI * 1318.5f * t + vib);
            float sparkle = 0.08f * sinf(TWO_PI * 2637 * t) * expf(-fabsf(t - 0.6f) / 0.12f);
            mix[start + i] += 0.32f * (chord * swell + sparkle);
        }
    }

    // 2) Turbo spooling through the needle sweep: a whistle climbing the whole
    //    time plus rushing air, cut off by the blow-off
    {
        int start = (int)(UI_SWEEP_DELAY_MS / 1000.0f * SR);
        float D = (UI_SWEEP_UP_MS + UI_SWEEP_HOLD_MS + UI_SWEEP_DOWN_MS) / 1000.0f;
        int n = (int)(D * SR);
        if (n > 2 * SR) n = 2 * SR;
        float ph = 0, hp_st = 0, lp_st = 0;
        for (int i = 0; i < n; i++) {
            float t = (float)i / SR, k = t / D, spool = 1 - powf(1 - k, 2.2f);
            float f = 1400 + 3800 * spool + 40 * sinf(TWO_PI * 7 * t) * spool;
            ph += TWO_PI * f / SR;
            if (ph > TWO_PI) ph -= TWO_PI;
            a[i] = (sinf(ph) + 0.25f * sinf(2 * ph + 0.4f)) * (0.05f + 0.95f * powf(spool, 1.3f));
            float x = noise();
            float h = x - lp_step(&hp_st, x, 600);                 // highpass 600 Hz
            b[i] = lp_step(&lp_st, h, 1500 + 5000 * spool);
        }
        normalize(b, n, 1.0f);
        int cut = (int)(0.02f * SR);
        for (int i = 0; i < n && start + i < N; i++) {
            float t = (float)i / SR, k = t / D, spool = 1 - powf(1 - k, 2.2f);
            float v = (0.16f * a[i] + 0.20f * b[i] * (0.15f + 0.85f * spool)) * fminf(1, t / 0.08f);
            if (i > n - cut) v *= 0.2f + 0.8f * (float)(n - i) / cut;
            mix[start + i] += v;
        }
    }

    // 3) HKS SSQV-style blow-off as the face lights: a quick first pop, then the
    //    main valve, with the trumpet's tonal "shhhuuuu" sweeping down
    {
        int start = (int)((UI_SWEEP_DELAY_MS + UI_SWEEP_UP_MS + UI_SWEEP_HOLD_MS + UI_SWEEP_DOWN_MS) / 1000.0f * SR);
        int n = SR;                                         // 1 s
        float st_hp = 0, st_lp = 0, st_hp2 = 0;
        bq_t bq = {0};
        float *pop = a, *tonal = b, *air = c;
        for (int i = 0; i < n; i++) {
            float t = (float)i / SR, t2 = fmaxf(0, t - 0.028f), x = noise();
            float h = x - lp_step(&st_hp, x, 2500);
            pop[i] = h * fminf(1, t / 0.0015f) * expf(-t / 0.018f);
            tonal[i] = bp_step(&bq, x, 4200 * expf(-t2 / 0.55f) + 1900, 9);
            float l = lp_step(&st_lp, x, 11000 * expf(-t2 / 0.5f) + 2500);
            air[i] = l - lp_step(&st_hp2, l, 1500);
        }
        normalize(pop, n, 1.0f); normalize(tonal, n, 1.0f); normalize(air, n, 1.0f);
        float peak = 0;
        float tph = 0;
        for (int i = 0; i < n; i++) {
            float t = (float)i / SR, t2 = fmaxf(0, t - 0.028f);
            float env = t >= 0.028f ? fminf(1, t2 / 0.004f) * expf(-t2 / 0.30f) : 0;
            float flutter = 1 + 0.08f * sinf(TWO_PI * 55 * t2) * expf(-t2 / 0.12f);
            a[i] = 0.55f * pop[i] + (air[i] * 0.75f + tonal[i] * 0.9f) * env * flutter;
            peak = fmaxf(peak, fabsf(a[i]));
        }
        for (int i = 0; i < n && start + i < N; i++) {
            float t = (float)i / SR;
            tph += TWO_PI * (5200 * expf(-t / 0.08f) + 300) / SR;
            float tail = 0.05f * sinf(tph) * expf(-t / 0.06f);       // last of the whistle
            mix[start + i] += a[i] * (0.55f / peak) + tail;
        }
    }

    for (int i = 0; i < N; i++) {
        float v = mix[i] * SOUND_STARTUP_GAIN;
        if (v > 1) v = 1;
        if (v < -1) v = -1;
        s_startup[i] = (int16_t)(v * 32767);
    }
    s_startup_len = N;
    free(mix); free(a); free(b); free(c);
    return true;
}


int main(void)
{
    (void)TAG;
    if (!render_startup()) return 1;
    printf("// Generated by scripts/startup_sound_synth.c: do not edit by hand.\n");
    printf("// Startup sound, mono 16-bit, %d Hz, %d samples (%.1f s).\n", SR, s_startup_len, (float)s_startup_len / SR);
    printf("#include <stdint.h>\n\n");
    printf("const int startup_sound_rate = %d;\n", SR);
    printf("const int startup_sound_len = %d;\n", s_startup_len);
    printf("const int16_t startup_sound_pcm[%d] = {\n", s_startup_len);
    for (int i = 0; i < s_startup_len; i++)
        printf("%d,%s", s_startup[i], (i % 16 == 15) ? "\n" : "");
    printf("\n};\n");
    return 0;
}
