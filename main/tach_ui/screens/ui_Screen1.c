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
static int32_t last_rpm = -1000;

// Shift light: 0 = off, 1 = yellow blink, 2 = red blink
static int shift_light_state = 0;
static bool shift_light_lit = false;
static lv_timer_t *shift_blink_timer = NULL;

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
    dot.bg_color = lv_color_hex(UI_COLOR_DIAL);
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

    // Units, tucked under the 4 and 5 like the OEM dial
    make_label(root, &ui_font_dial_30, UI_COLOR_DIAL, "x1000r/min", 0, UI_SCALE(-214));

    return root;
}

void ui_Screen1_screen_init(void)
{
    ui_Screen1 = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_Screen1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_Screen1, lv_color_hex(0x0B0B0B), 0);
    lv_obj_set_style_bg_opa(ui_Screen1, LV_OPA_COVER, 0);

    // ---- Static dial -> one image ----
    lv_obj_t *dial = build_static_dial(ui_Screen1);
    lv_obj_update_layout(ui_Screen1);
#if LV_USE_SNAPSHOT && UI_DIAL_USE_SNAPSHOT
    dial_snapshot = lv_snapshot_take(dial, LV_IMG_CF_TRUE_COLOR);
    if (dial_snapshot) {
        lv_obj_del(dial);
        lv_obj_t *img = lv_img_create(ui_Screen1);
        lv_img_set_src(img, dial_snapshot);
        lv_obj_center(img);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    }
#endif

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
}

void ui_tach_set_rpm(int32_t rpm)
{
    if (!ui_rpm_meter) return;
    if (rpm < 0) rpm = 0;
    if (rpm > UI_TACH_MAX_RPM) rpm = UI_TACH_MAX_RPM;

    // ~0.5 degree of needle travel; avoids redraws for sensor noise
    if (LV_ABS(rpm - last_rpm) >= 15) {
        lv_meter_set_indicator_value(ui_rpm_meter, needle_glow, rpm);
        lv_meter_set_indicator_value(ui_rpm_meter, needle, rpm);
        last_rpm = rpm;
    }

    // Shift light, with 100 rpm hysteresis on the way down so it doesn't
    // chatter between states when RPM hovers right at a threshold
    int target = rpm >= UI_TACH_SHIFT_RPM ? 2 : rpm >= UI_SHIFT_WARN_RPM ? 1 : 0;
    if (target < shift_light_state) {
        int hold_above = (shift_light_state == 2 ? UI_TACH_SHIFT_RPM : UI_SHIFT_WARN_RPM) - 100;
        if (rpm >= hold_above) target = shift_light_state;
    }
    shift_light_set_state(target);
}

void ui_Screen1_screen_destroy(void)
{
    if (shift_blink_timer) lv_timer_del(shift_blink_timer);
    shift_blink_timer = NULL;
    shift_light_state = 0;
    if (ui_Screen1) lv_obj_del(ui_Screen1);
#if LV_USE_SNAPSHOT
    if (dial_snapshot) lv_snapshot_free(dial_snapshot);
#endif
    dial_snapshot = NULL;
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
