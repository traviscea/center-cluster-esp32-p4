// Analog-style tach screen: black face, red-orange backlit ticks/numerals,
// needle with a center hub, stippled redline, and digital speed + gear.
//
// Supports 800x800 and 720x720 round displays automatically (3.4in / 4in).
//
// Rendering strategy (keeps the frame rate up on the ESP32-P4):
//   * Everything that never changes (face, ticks, numerals, units text)
//     is built once and flattened into a single image with lv_snapshot.
//   * Only the needle meter, hub, gear and speed labels sit on top of it, so a
//     needle move just re-blits a slice of that image instead of re-running
//     every tick/label/mask draw.
//   If the snapshot can't be allocated, the live widgets are simply kept.

#include "../ui.h"
#include <math.h>

LV_IMG_DECLARE(ui_img_sti_badge);        // images/ui_img_sti_badge.c
LV_IMG_DECLARE(ui_img_sti_badge_glow);

static lv_obj_t *badge_obj = NULL, *badge_glow = NULL;

// Base resolution the layout is designed for (800x800 round screen)
#define UI_BASE_RES 800
#define UI_SCALE(x) ((x) * LV_HOR_RES / UI_BASE_RES)

// ---- Dial geometry (at 800px) ----
#define TICK_D         784          // outer edge of the tick marks (fills the round screen)
#define DIAL_ROTATION  135          // 0 rpm at ~7:30 (degrees, clockwise from 3 o'clock)
#define DIAL_SWEEP     270          // 0..9 sweeps to ~4:30
#define HUB_D          176

#define DEG_PER_RPM(r) ((r) * DIAL_SWEEP / UI_TACH_MAX_RPM)

lv_obj_t *ui_Screen1 = NULL;
lv_obj_t *ui_rpm_meter = NULL;
lv_obj_t *ui_rpm_hub = NULL;
lv_obj_t *ui_shift_light = NULL;
lv_obj_t *ui_label_mph_value = NULL;
lv_obj_t *ui_label_gear_value = NULL;
lv_obj_t *ui_label_odometer_value = NULL;

static lv_meter_indicator_t *needle = NULL;
static lv_meter_indicator_t *needle_glow = NULL;
static lv_img_dsc_t *dial_snapshot = NULL;
static lv_obj_t *dial_obj = NULL;         // the face: snapshot image or live widgets
static int32_t last_rpm = -1000;

// Shift light: 0 = off, 1 = yellow blink, 2 = red blink
static int shift_light_state = 0;
static bool shift_light_lit = false;
static lv_timer_t *shift_blink_timer = NULL;

static lv_obj_t *status_label = NULL;
static lv_timer_t *status_timer = NULL;

// Meter labels are the raw value (1000, 2000...). Show 1, 2, 3... instead.
static void dial_label_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (dsc->class_p == &lv_meter_class && dsc->type == LV_METER_DRAW_PART_TICK && dsc->text) {
        lv_snprintf(dsc->text, 16, "%d", (int)(dsc->value / 1000));
    }
}

// Stippled redline band: staggered rows of small dots along the outer edge.
// Drawn once into the dial snapshot, so the cost doesn't matter at runtime.
static void redline_stipple_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);

    lv_area_t a;
    lv_obj_get_content_coords(obj, &a);
    float cx = a.x1 + lv_area_get_width(&a) / 2.0f;
    float cy = a.y1 + lv_area_get_height(&a) / 2.0f;
    float r_out = lv_area_get_width(&a) / 2.0f;

    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.radius = LV_RADIUS_CIRCLE;
    dot.bg_color = lv_color_hex(UI_COLOR_REDLINE);
    dot.bg_opa = LV_OPA_COVER;

    const float deg2rad = 3.14159265f / 180.0f;
    float a0 = (DIAL_ROTATION + (float)UI_TACH_REDLINE_RPM * DIAL_SWEEP / UI_TACH_MAX_RPM) * deg2rad;
    float a1 = (DIAL_ROTATION + DIAL_SWEEP) * deg2rad;
    float pitch = UI_SCALE(7);          // dot spacing in px
    lv_coord_t ds = UI_SCALE(4);        // dot size in px
    int rows = 5;

    for (int row = 0; row < rows; row++) {
        float r = r_out - UI_SCALE(5) - row * pitch;
        float step = pitch / r;
        float start = a0 + ((row & 1) ? step / 2 : 0);
        for (float ang = start; ang <= a1 + 0.0001f; ang += step) {
            lv_area_t d;
            d.x1 = (lv_coord_t)(cx + r * cosf(ang) - ds / 2.0f);
            d.y1 = (lv_coord_t)(cy + r * sinf(ang) - ds / 2.0f);
            d.x2 = d.x1 + ds - 1;
            d.y2 = d.y1 + ds - 1;
            lv_draw_rect(draw_ctx, &dot, &d);
        }
    }
}

static void shift_light_paint(bool lit)
{
    shift_light_lit = lit;
    if (!lit) {
        // Unlit lens, like the stock lamp when it's off
        lv_obj_set_style_bg_color(ui_shift_light, lv_color_hex(0x161616), 0);
        lv_obj_set_style_border_color(ui_shift_light, lv_color_hex(0x262626), 0);
        lv_obj_set_style_outline_opa(ui_shift_light, LV_OPA_TRANSP, 0);
        return;
    }
    lv_color_t c = lv_color_hex(shift_light_state == 2 ? UI_COLOR_SHIFT_RED : UI_COLOR_SHIFT_WARN);
    lv_obj_set_style_bg_color(ui_shift_light, c, 0);
    lv_obj_set_style_border_color(ui_shift_light, c, 0);
    lv_obj_set_style_outline_color(ui_shift_light, c, 0);
    lv_obj_set_style_outline_opa(ui_shift_light, LV_OPA_40, 0);
}

static void shift_blink_cb(lv_timer_t *t)
{
    (void)t;
    if (shift_light_state) shift_light_paint(!shift_light_lit);
}

static void shift_light_set_state(int state)
{
    if (state == shift_light_state) return;
    shift_light_state = state;
    if (state == 0) {
        lv_timer_pause(shift_blink_timer);
        shift_light_paint(false);
    } else {
        lv_timer_set_period(shift_blink_timer, state == 2 ? UI_SHIFT_BLINK_RED_MS : UI_SHIFT_BLINK_WARN_MS);
        lv_timer_reset(shift_blink_timer);
        lv_timer_resume(shift_blink_timer);
        shift_light_paint(true);   // light up immediately, then blink
    }
}

// ---------------------------------------------------------------- face lighting
// During the startup sweep only the needles are lit; the rest of the face is
// dark and fades in once the sweep ends, like the stock cluster at key-on.

static lv_opa_t face_opa = LV_OPA_COVER;

static void face_set_opa(lv_opa_t o)
{
    face_opa = o;
    if (!ui_Screen1) return;
    uint32_t n = lv_obj_get_child_cnt(ui_Screen1);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *c = lv_obj_get_child(ui_Screen1, i);
        if (c == ui_rpm_meter || c == badge_obj || c == badge_glow) continue;               // needles stay lit
        if (c == ui_rpm_hub) {                          // hub caps aren't lit; their numbers are
            uint32_t k = lv_obj_get_child_cnt(c);
            for (uint32_t j = 0; j < k; j++) lv_obj_set_style_opa(lv_obj_get_child(c, j), o, 0);
            continue;
        }
        lv_obj_set_style_opa(c, o, 0);
    }
}

static void face_fade_cb(void *var, int32_t v)
{
    (void)var;
    face_set_opa((lv_opa_t)v);
}

static void face_fade_in(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ui_Screen1);
    lv_anim_set_exec_cb(&a, face_fade_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, UI_SWEEP_FADE_MS);
    lv_anim_start(&a);
}

// ---------------------------------------------------------------- backlight
// The backlight comes on at a set point in the startup (UI_BACKLIGHT_ON_MS),
// on the same timeline on all three screens, so they light up together.

static void (*backlight_cb)(void) = NULL;
static bool backlight_done = false;

void ui_set_backlight_cb(void (*cb)(void)) { backlight_cb = cb; }

static void backlight_on(void)
{
    if (backlight_done) return;
    backlight_done = true;
    if (backlight_cb) backlight_cb();
}

// ---------------------------------------------------------------- startup sweep
// At boot the needle sweeps to full scale and back down to the live RPM, like
// the stock cluster at key-on. While it runs, ui_tach_set_rpm() only records
// the value, so the sweep lands on the real RPM when it finishes.

static lv_timer_t *sweep_timer = NULL;
static uint32_t sweep_start = 0;
static int32_t sweep_live_rpm = 0;

static float sweep_ease(float x)              // smooth start and stop
{
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    return x * x * (3 - 2 * x);
}

// Needle position at time t (ms into the sweep) on a 0..full scale, ending at
// 'rest'. Returns false once the sweep is over.
static bool sweep_pos(uint32_t t, float full, float rest, float *out)
{
    const uint32_t up = UI_SWEEP_UP_MS, hold = UI_SWEEP_HOLD_MS, down = UI_SWEEP_DOWN_MS;
    if (t < up) { *out = full * sweep_ease((float)t / up); return true; }
    if (t < up + hold) { *out = full; return true; }
    if (t < up + hold + down) {
        *out = full + (rest - full) * sweep_ease((float)(t - up - hold) / down);
        return true;
    }
    return false;
}

// Badge intro: fade up with a bright glow, then the glow eases down to rest
static void logo_step(uint32_t el)
{
    static lv_opa_t shown_badge = 1, shown_glow = 1;    // forces the first update
    lv_opa_t b = LV_OPA_COVER, g = UI_LOGO_GLOW_REST;
    if (el >= UI_BACKLIGHT_ON_MS) backlight_on();
    if (el < UI_LOGO_DELAY_MS) {
        b = g = LV_OPA_TRANSP;
    } else if (el < UI_LOGO_DELAY_MS + UI_LOGO_FADE_MS) {
        b = g = (lv_opa_t)(255 * sweep_ease((float)(el - UI_LOGO_DELAY_MS) / UI_LOGO_FADE_MS));
    } else if (el < UI_LOGO_DELAY_MS + UI_LOGO_FADE_MS + UI_LOGO_SETTLE_MS) {
        float k = sweep_ease((float)(el - UI_LOGO_DELAY_MS - UI_LOGO_FADE_MS) / UI_LOGO_SETTLE_MS);
        g = (lv_opa_t)(255 + (UI_LOGO_GLOW_REST - 255) * k);
    }
    if (b != shown_badge) { lv_obj_set_style_opa(badge_obj, b, 0); shown_badge = b; }
    if (g != shown_glow) { lv_obj_set_style_opa(badge_glow, g, 0); shown_glow = g; }
}

uint16_t ui_startup_ms(void)
{
#if UI_SWEEP_ENABLE
    uint32_t el = lv_tick_elaps(sweep_start);
    return el > 0xFFFF ? 0xFFFF : (uint16_t)el;
#else
    return 0xFFFF;                            // no startup sequence
#endif
}

static void sweep_cb(lv_timer_t *t)
{
    (void)t;
    uint32_t el = lv_tick_elaps(sweep_start);
    logo_step(el);
    if (el < UI_SWEEP_DELAY_MS) return;
    el -= UI_SWEEP_DELAY_MS;

    if (lv_obj_has_flag(ui_rpm_meter, LV_OBJ_FLAG_HIDDEN)) {   // needle and hub appear
        lv_obj_clear_flag(ui_rpm_meter, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_rpm_hub, LV_OBJ_FLAG_HIDDEN);
    }

    float rpm;
    if (sweep_pos(el, UI_TACH_MAX_RPM, (float)sweep_live_rpm, &rpm)) {
        lv_meter_set_indicator_value(ui_rpm_meter, needle_glow, (int32_t)rpm);
        lv_meter_set_indicator_value(ui_rpm_meter, needle, (int32_t)rpm);
        return;
    }

    // Done: hand the needle and shift light back to live RPM
    lv_timer_del(sweep_timer);
    sweep_timer = NULL;
    shift_light_state = 0;
    shift_light_paint(false);
    last_rpm = -1000;
    ui_tach_set_rpm(sweep_live_rpm);
    logo_step(UINT32_MAX / 2);                // badge and glow at rest
    backlight_on();                           // (already on; just in case)
    face_fade_in();
}

static lv_obj_t *make_badge(const lv_img_dsc_t *src)
{
    lv_obj_t *b = lv_img_create(ui_Screen1);
    lv_img_set_src(b, src);
    lv_obj_set_style_img_recolor(b, lv_color_hex(UI_COLOR_STI_BADGE), 0);
    lv_obj_set_style_img_recolor_opa(b, LV_OPA_COVER, 0);
    lv_img_set_zoom(b, (uint16_t)(256 * LV_HOR_RES / UI_BASE_RES));
    lv_obj_clear_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, UI_SCALE(-214));
    return b;
}

static lv_obj_t *make_circle(lv_obj_t *parent, lv_coord_t d)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, d, d);
    lv_obj_center(o);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                            const char *txt, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_CENTER, x, y);
    return l;
}

static void style_bare_meter(lv_obj_t *m)
{
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, UI_SCALE(TICK_D), UI_SCALE(TICK_D));
    lv_obj_center(m);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    // Hide the default center dot; the hub replaces it
    lv_obj_set_style_width(m, 0, LV_PART_INDICATOR);
    lv_obj_set_style_height(m, 0, LV_PART_INDICATOR);
}

// Builds every static part of the dial inside one full-screen container
static lv_obj_t *build_static_dial(lv_obj_t *parent)
{
    lv_color_t dial = lv_color_hex(UI_COLOR_DIAL);

    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_HOR_RES, LV_VER_RES);
    lv_obj_center(root);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0B0B0B), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *m = lv_meter_create(root);
    style_bare_meter(m);
    lv_obj_set_style_text_font(m, &ui_font_dial_74, LV_PART_TICKS);
    lv_obj_set_style_text_color(m, dial, LV_PART_TICKS);
    lv_obj_add_event_cb(m, dial_label_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    // 250 rpm ticks (short)
    lv_meter_scale_t *s250 = lv_meter_add_scale(m);
    lv_meter_set_scale_range(m, s250, 0, UI_TACH_MAX_RPM, DIAL_SWEEP, DIAL_ROTATION);
    lv_meter_set_scale_ticks(m, s250, UI_TACH_MAX_RPM / 250 + 1, UI_SCALE(3), UI_SCALE(16), dial);

    // 500 rpm ticks (medium)
    lv_meter_scale_t *s500 = lv_meter_add_scale(m);
    lv_meter_set_scale_range(m, s500, 0, UI_TACH_MAX_RPM, DIAL_SWEEP, DIAL_ROTATION);
    lv_meter_set_scale_ticks(m, s500, UI_TACH_MAX_RPM / 500 + 1, UI_SCALE(4), UI_SCALE(26), dial);

    // 1000 rpm ticks (long) + numerals
    lv_meter_scale_t *s1k = lv_meter_add_scale(m);
    lv_meter_set_scale_range(m, s1k, 0, UI_TACH_MAX_RPM, DIAL_SWEEP, DIAL_ROTATION);
    lv_meter_set_scale_ticks(m, s1k, UI_TACH_MAX_RPM / 1000 + 1, UI_SCALE(7), UI_SCALE(44), dial);
    lv_meter_set_scale_major_ticks(m, s1k, 1, UI_SCALE(7), UI_SCALE(44), dial, UI_SCALE(36));

    // Redline: stippled dot band along the outer edge (under the ticks)
    lv_obj_add_event_cb(m, redline_stipple_cb, LV_EVENT_DRAW_MAIN_BEGIN, NULL);


    return root;
}

static void recolor_live_parts(void)
{
    // Gear and odometer are live labels; main.c also passes the current theme
    // colors on every update, so they stay in sync.
    if (ui_label_gear_value) lv_obj_set_style_text_color(ui_label_gear_value, lv_color_hex(UI_COLOR_DIAL), 0);
    if (ui_label_odometer_value) lv_obj_set_style_text_color(ui_label_odometer_value, lv_color_hex(UI_COLOR_DIAL_DIM), 0);
}

// ---------------------------------------------------------------- color themes

// Must be the same list, in the same order, on all three screens.
const ui_theme_t ui_themes[UI_THEME_COUNT] = {
    { "RED",    0xFF2A0A, 0xE6260A },
    { "WHITE",  0xFFFFFF, 0xD6DEE6 },
    { "GREEN",  0x00FF55, 0x00D648 },
    { "BLUE",   0x00AAFF, 0x0090DD },
    { "PURPLE", 0xD24DFF, 0xB43FDD },
    { "AMBER",  0xFFB300, 0xE69E00 },
};
static uint8_t theme_index = 0;

uint32_t ui_theme_dial(void) { return ui_themes[theme_index].dial; }
uint32_t ui_theme_dim(void)  { return ui_themes[theme_index].dim; }
uint8_t  ui_get_theme(void)  { return theme_index; }

// Build the static face (ticks, numerals, labels) in the current theme and
// flatten it into one image. Used at start-up and again on a theme change, when
// the new face replaces the old one in place behind everything else.
static void install_static_dial(void)
{
#if LV_USE_SNAPSHOT && UI_DIAL_USE_SNAPSHOT
    // Free the old face first so only one full-screen image is in memory at once.
    // (Nothing is drawn in between: this all runs under the LVGL lock.)
    if (dial_snapshot) {
        if (dial_obj) { lv_obj_del(dial_obj); dial_obj = NULL; }
        lv_img_cache_invalidate_src(dial_snapshot);
        lv_snapshot_free(dial_snapshot);
        dial_snapshot = NULL;
    }
#endif
    lv_obj_t *dial = build_static_dial(ui_Screen1);
    lv_obj_update_layout(ui_Screen1);
#if LV_USE_SNAPSHOT && UI_DIAL_USE_SNAPSHOT
    dial_snapshot = lv_snapshot_take(dial, LV_IMG_CF_TRUE_COLOR);
    if (dial_snapshot) {
        lv_obj_del(dial);
        dial = lv_img_create(ui_Screen1);
        lv_img_set_src(dial, dial_snapshot);
        lv_obj_center(dial);
        lv_obj_clear_flag(dial, LV_OBJ_FLAG_CLICKABLE);
    }
#endif
    if (dial_obj) lv_obj_del(dial_obj);       // old live face (snapshot off/failed)
    dial_obj = dial;
    lv_obj_move_background(dial_obj);
    if (face_opa != LV_OPA_COVER) lv_obj_set_style_opa(dial_obj, face_opa, 0);   // theme change mid-sweep
}

void ui_set_theme(uint8_t index)
{
    if (index >= UI_THEME_COUNT || index == theme_index) return;
    theme_index = index;
    if (!ui_Screen1) return;                  // not built yet: the first build uses it
    install_static_dial();
    recolor_live_parts();
}

void ui_Screen1_screen_init(void)
{
    ui_Screen1 = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Screen1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Screen1, lv_color_hex(0x0B0B0B), 0);
    lv_obj_set_style_bg_opa(ui_Screen1, LV_OPA_COVER, 0);

    // ---- Static dial -> one image ----
    dial_obj = NULL;
    install_static_dial();

    // STI badge where the OEM dial has its units, under the 4 and 5, with a soft
    // glow under it. Alpha masks, so recolor sets the color: pink on every theme.
    // Kept out of the dial image so it can light up first at startup.
    badge_glow = make_badge(&ui_img_sti_badge_glow);
    badge_obj = make_badge(&ui_img_sti_badge);
    lv_obj_set_style_opa(badge_glow, UI_LOGO_GLOW_REST, 0);

    // ---- Shift light: stock STI position, just above the hub (under the needle) ----
    ui_shift_light = make_circle(ui_Screen1, UI_SCALE(38));
    lv_obj_align(ui_shift_light, LV_ALIGN_CENTER, 0, UI_SCALE(-136));
    lv_obj_set_style_border_width(ui_shift_light, UI_SCALE(3), 0);
    lv_obj_set_style_outline_width(ui_shift_light, UI_SCALE(7), 0);
    lv_obj_set_style_outline_pad(ui_shift_light, 0, 0);
    shift_light_state = 0;
    shift_light_paint(false);
    shift_blink_timer = lv_timer_create(shift_blink_cb, UI_SHIFT_BLINK_WARN_MS, NULL);
    lv_timer_pause(shift_blink_timer);

    // ---- Needle layer ----
    ui_rpm_meter = lv_meter_create(ui_Screen1);
    style_bare_meter(ui_rpm_meter);
    lv_meter_scale_t *ns = lv_meter_add_scale(ui_rpm_meter);
    lv_meter_set_scale_range(ui_rpm_meter, ns, 0, UI_TACH_MAX_RPM, DIAL_SWEEP, DIAL_ROTATION);
    lv_meter_set_scale_ticks(ui_rpm_meter, ns, 0, 0, 0, lv_color_black());

    // Soft glow under the needle, then the needle itself
    needle_glow = lv_meter_add_needle_line(ui_rpm_meter, ns, UI_SCALE(18),
                                           lv_color_hex(UI_COLOR_NEEDLE), -UI_SCALE(26));
    needle_glow->opa = LV_OPA_30;
    needle = lv_meter_add_needle_line(ui_rpm_meter, ns, UI_SCALE(8),
                                      lv_color_hex(UI_COLOR_NEEDLE), -UI_SCALE(22));
    lv_meter_set_indicator_value(ui_rpm_meter, needle_glow, 0);
    lv_meter_set_indicator_value(ui_rpm_meter, needle, 0);

    // ---- Center hub (gear lives here) ----
    ui_rpm_hub = make_circle(ui_Screen1, UI_SCALE(HUB_D));
    lv_obj_set_style_bg_color(ui_rpm_hub, lv_color_hex(0x202123), 0);
    lv_obj_set_style_bg_grad_color(ui_rpm_hub, lv_color_hex(0x0A0A0B), 0);
    lv_obj_set_style_bg_grad_dir(ui_rpm_hub, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(ui_rpm_hub, UI_SCALE(4), 0);
    lv_obj_set_style_border_color(ui_rpm_hub, lv_color_hex(0x3C3E41), 0);

    ui_label_gear_value = make_label(ui_rpm_hub, &ui_font_gear_136, UI_COLOR_DIAL, "N", 0, UI_SCALE(4));

    // ---- Speed in the open wedge below the hub ----
    ui_label_mph_value = make_label(ui_Screen1, &ui_font_speed_156, UI_COLOR_SPEED, "--", 0, UI_SCALE(196));
    ui_label_odometer_value = make_label(ui_Screen1, &ui_font_odo_42, UI_COLOR_DIAL_DIM, "000000.0", 0, UI_SCALE(330));

    last_rpm = -1000;

    backlight_done = false;
#if !UI_SWEEP_ENABLE
    backlight_on();                           // no startup sequence: light up now
#endif
#if UI_SWEEP_ENABLE
    sweep_live_rpm = 0;
    sweep_start = lv_tick_get();
    sweep_timer = lv_timer_create(sweep_cb, 10, NULL);
    face_set_opa(LV_OPA_TRANSP);              // blank face
    lv_obj_add_flag(ui_rpm_meter, LV_OBJ_FLAG_HIDDEN);   // needle and hub come in after the badge
    lv_obj_add_flag(ui_rpm_hub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(badge_obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_opa(badge_glow, LV_OPA_TRANSP, 0);
#endif
}

void ui_tach_set_rpm(int32_t rpm)
{
    if (!ui_rpm_meter) return;
    if (rpm < 0) rpm = 0;
    if (rpm > UI_TACH_MAX_RPM) rpm = UI_TACH_MAX_RPM;
    if (sweep_timer) { sweep_live_rpm = rpm; return; }   // startup sweep owns the needle

    // ~0.5 degree of needle travel; avoids redraws for sensor noise
    if (LV_ABS(rpm - last_rpm) >= 15) {
        lv_meter_set_indicator_value(ui_rpm_meter, needle_glow, rpm);
        lv_meter_set_indicator_value(ui_rpm_meter, needle, rpm);
        last_rpm = rpm;
    }
}

void ui_tach_set_shift_rpm(int32_t rpm)
{
    if (!ui_shift_light || sweep_timer) return;   // stays dark during the startup sweep

    // Shift light, with 100 rpm hysteresis on the way down so it doesn't
    // chatter between states when RPM hovers right at a threshold
    int target = rpm >= UI_TACH_SHIFT_RPM ? 2 : rpm >= UI_SHIFT_WARN_RPM ? 1 : 0;
    if (target < shift_light_state) {
        int hold_above = (shift_light_state == 2 ? UI_TACH_SHIFT_RPM : UI_SHIFT_WARN_RPM) - 100;
        if (rpm >= hold_above) target = shift_light_state;
    }
    shift_light_set_state(target);
}

static void status_hide_cb(lv_timer_t *t)
{
    (void)t;
    if (status_label) lv_obj_del(status_label);
    status_label = NULL;
    status_timer = NULL;          // one-shot timer deletes itself
}

void ui_tach_show_status(const char *text, uint32_t duration_ms)
{
    if (!ui_Screen1) return;
    if (!status_label) {
        status_label = lv_label_create(ui_Screen1);
        lv_obj_set_style_opa(status_label, face_opa, 0);   // lights up with the face
        lv_obj_set_style_text_font(status_label, &ui_font_status_26, 0);
        lv_obj_set_style_text_color(status_label, lv_color_hex(UI_COLOR_SHIFT_WARN), 0);
        lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_label_set_text(status_label, text);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, UI_SCALE(362));   // just below the odometer

    if (status_timer) lv_timer_del(status_timer);
    status_timer = lv_timer_create(status_hide_cb, duration_ms, NULL);
    lv_timer_set_repeat_count(status_timer, 1);
}

void ui_Screen1_screen_destroy(void)
{
    if (status_timer) lv_timer_del(status_timer);
    status_timer = NULL;
    status_label = NULL;
    if (shift_blink_timer) lv_timer_del(shift_blink_timer);
    shift_blink_timer = NULL;
    if (sweep_timer) lv_timer_del(sweep_timer);
    sweep_timer = NULL;
    shift_light_state = 0;
    badge_obj = badge_glow = NULL;
    if (ui_Screen1) lv_obj_del(ui_Screen1);
#if LV_USE_SNAPSHOT
    if (dial_snapshot) lv_snapshot_free(dial_snapshot);
#endif
    dial_snapshot = NULL;
    dial_obj = NULL;
    needle = NULL;
    needle_glow = NULL;

    ui_Screen1 = NULL;
    ui_rpm_meter = NULL;
    ui_rpm_hub = NULL;
    ui_shift_light = NULL;
    ui_label_mph_value = NULL;
    ui_label_gear_value = NULL;
    ui_label_odometer_value = NULL;
}
