#include "sound.h"

#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "bsp_board_extra.h"
#include "ui.h"               // ui_startup_ms(), UI_* startup timings

static const char *TAG = "sound";

#define SR            32000            // sample rate (Hz): enough for the hiss and whistle
#define CHUNK         256              // frames written to the codec per loop (8 ms)
#define TWO_PI        6.28318530718f

static volatile bool s_shift_on = false;
static volatile bool s_alert_on = false;

void sound_set_shift(bool on) { s_shift_on = on; }
void sound_set_alert(bool on) { s_alert_on = on; }

// ---------------------------------------------------------------- startup clip
// Pre-rendered (startup_sound_pcm.c, made by scripts/startup_sound_synth.c) and
// played from the point matching the screen's startup clock, so it stays
// locked to the badge and the sweep.

extern const int startup_sound_rate;
extern const int startup_sound_len;
extern const int16_t startup_sound_pcm[];

// ---------------------------------------------------------------- shift buzzer

static float shift_sample(uint32_t n)        // n = samples since the buzzer started
{
    uint32_t period = SOUND_SHIFT_PERIOD_MS * SR / 1000, on = SOUND_SHIFT_ON_MS * SR / 1000;
    uint32_t p = n % period;
    if (p >= on) return 0;
    float t = (float)p / SR, d = (float)on / SR, f = SOUND_SHIFT_HZ;
    float s1 = sinf(TWO_PI * f * t) >= 0 ? 1.0f : -1.0f;
    float s2 = sinf(TWO_PI * f * t + 0.25f) >= 0 ? 1.0f : -1.0f;
    float x = (s1 - 0.6f * s2 + 0.5f * sinf(TWO_PI * f / 2 * t)) / 2.1f;   // piezo-like, with some body
    float env = fminf(1, t / 0.0015f) * fminf(1, (d - t) / 0.002f);
    return 0.95f * SOUND_SHIFT_GAIN * x * env;          // near full scale
}

// ---------------------------------------------------------------- warning beeps
// Three quick high beeps, then a pause, repeating: clearly different from the
// shift buzzer (lower, steady beep-beep).

static float alert_sample(uint32_t n)
{
    const uint32_t period = SR * 8 / 10;               // 0.8 s
    const uint32_t pip = SR * 9 / 100, step = SR * 15 / 100;
    uint32_t p = n % period;
    if (p >= 3 * step) return 0;
    uint32_t q = p % step;
    if (q >= pip) return 0;
    float t = (float)q / SR, d = (float)pip / SR, f = SOUND_ALERT_HZ;
    float x = (sinf(TWO_PI * f * t) + 0.25f * sinf(TWO_PI * 3 * f * t)) / 1.25f;
    float env = fminf(1, t / 0.002f) * fminf(1, (d - t) / 0.006f);
    return 1.36f * SOUND_ALERT_GAIN * x * env;          // near full scale (x peaks ~0.7)
}

// ---------------------------------------------------------------- task

static void sound_task(void *arg)
{
    (void)arg;
    if (bsp_extra_codec_init() != ESP_OK) {
        ESP_LOGE(TAG, "codec init failed: no sound");
        vTaskDelete(NULL);
    }
    bsp_extra_codec_set_fs(SR, 16, I2S_SLOT_MODE_STEREO);
    bsp_extra_codec_volume_set(SOUND_VOLUME, NULL);
    bool have_startup = (startup_sound_rate == SR);
    static int16_t buf[CHUNK * 2];                     // stereo
    int startup_pos = -1;                              // -1 = not started, >= len = done
    uint32_t shift_n = 0, alert_n = 0;
    bool shift_was_on = false, alert_was_on = false;

    while (1) {
        // Start the clip from the point that matches the screen's startup clock
        if (have_startup && startup_pos < 0) {
            uint32_t ms = ui_startup_ms();
            if (ms < 0xFFFF) startup_pos = (int)((uint64_t)ms * SR / 1000);
            else startup_pos = startup_sound_len;       // screen started long ago: skip it
        }
        bool alert = s_alert_on;
        bool shift = s_shift_on && !alert;             // a warning takes priority
        if (shift && !shift_was_on) shift_n = 0;       // each time it comes on, start with a beep
        if (alert && !alert_was_on) alert_n = 0;
        shift_was_on = shift;
        alert_was_on = alert;

        for (int i = 0; i < CHUNK; i++) {
            float v = 0;
            if (have_startup && startup_pos >= 0 && startup_pos < startup_sound_len)
                v += startup_sound_pcm[startup_pos++] / 32768.0f * SOUND_STARTUP_GAIN;
            if (shift) v += shift_sample(shift_n++);
            if (alert) v += alert_sample(alert_n++);
            if (v > 1) v = 1;
            if (v < -1) v = -1;
            int16_t s = (int16_t)(v * 32767);
            buf[2 * i] = s;
            buf[2 * i + 1] = s;
        }
        size_t written = 0;
        bsp_extra_i2s_write(buf, sizeof(buf), &written, portMAX_DELAY);   // paces the loop
        if (have_startup && startup_pos >= startup_sound_len) have_startup = false;   // clip done
    }
}

void sound_init(void)
{
#if SOUND_ENABLE
    // High priority so the audio never runs dry (a gap would click); it only
    // wakes when the codec needs the next 8 ms of samples.
    xTaskCreatePinnedToCore(sound_task, "sound", 6144, NULL, 6, NULL, 1);
#endif
}
