// Center-cluster sounds through the board's speaker (ES8311 codec):
//  - startup: shimmer as the STI badge glows in, a turbo spooling up through
//    the needle sweep, and an HKS SSQV-style blow-off as the face lights up.
//    Locked to the screen's startup clock (ui_startup_ms()).
//  - shift: a stock GD STI-style REV buzzer, beeping while at/above the red
//    shift point.
//  - alert: a fast triple beep, repeating while a warning (oil pressure, oil
//    temp, coolant) is active. It takes priority over the shift buzzer.
#pragma once
#include <stdbool.h>

// ---------------- Tune these ----------------
#define SOUND_ENABLE           1      // 0 = no sound at all
#define SOUND_VOLUME           100    // speaker (codec) volume, 0-100 (100 = max)
#define SOUND_STARTUP_GAIN     0.85f  // startup sound level, 0-1
#define SOUND_SHIFT_GAIN       1.00f  // shift buzzer level, 0-1 (kept louder than the startup)
#define SOUND_SHIFT_HZ         1850   // shift buzzer pitch
#define SOUND_SHIFT_ON_MS      110    // beep length...
#define SOUND_SHIFT_PERIOD_MS  220    // ...repeated this often while above the shift point
#define SOUND_ALERT_GAIN       1.00f  // warning beeps level, 0-1
#define SOUND_ALERT_HZ         2000   // warning beep pitch

// Start the speaker and the sound task. Call once, right after ui_init(): the
// startup sound follows the screen's startup clock from then on.
void sound_init(void);

// Turn the shift buzzer on/off (safe from any task; cheap to call every frame).
void sound_set_shift(bool on);

// Turn the warning beeps on/off (safe from any task; cheap to call every frame).
void sound_set_alert(bool on);
