#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_memory_utils.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "bsp_board_extra.h"
#include "lvgl.h"
#include "lv_demos.h"
#include "ui.h"


float tach_rpm_angle = 0;       
bool tach_rpm_angle_increasing = true;
int speed_mph = 34;
bool speed_mph_increasing = true;

static lv_color_t green_color;
static lv_color_t red_color;
static lv_color_t orange_color;

static inline int constrain_int(int x, int low, int high) {
    if (x < low) return low;
    if (x > high) return high;
    return x;
}

static void init_label_styles(void){

    green_color = lv_palette_main(LV_PALETTE_GREEN);
    red_color = lv_palette_main(LV_PALETTE_RED);
    orange_color = lv_palette_main(LV_PALETTE_DEEP_ORANGE);
}

static void update_label_if_needed(lv_obj_t *label, int new_value, lv_color_t new_color) {
    char buf[12];
    sprintf(buf, "%d", new_value);

    // Only update text if changed
    const char *old_text = lv_label_get_text(label);
    if (strcmp(old_text, buf) != 0) {
        lv_label_set_text(label, buf);
    }

    // Only update color if changed
    lv_color_t old_color = lv_obj_get_style_text_color(label, LV_PART_MAIN);
    if (old_color.full != new_color.full) {
        lv_obj_set_style_text_color(label, new_color, LV_PART_MAIN);
    }

}

void simulate_guage_movement(int gauge_value, int max_value, int min_value, int step, lv_obj_t * text, const char * type, bool isIncreasing)
{
    
    gauge_value += (isIncreasing ? step : -step);
    lv_color_t color = green_color;
    
    if (gauge_value >= max_value || gauge_value <= min_value){
        isIncreasing = !isIncreasing;
        gauge_value = constrain_int(gauge_value, 0, max_value);
    }
    // if(strcmp(type, "speed_mph") == 0) {
         speed_mph = gauge_value;
         speed_mph_increasing = isIncreasing;
    //     color = gauge_value > 60 ? red_color : green_color;
    // }

    update_label_if_needed(text, gauge_value, color);
}

static void simulate_tach_movement(lv_obj_t *tach, int step) {
    // move needle in the current direction
    tach_rpm_angle += (tach_rpm_angle_increasing ? step : -step);

    // reverse direction if we hit either end
    if (tach_rpm_angle >= 100) {  // reached rightmost
        tach_rpm_angle_increasing = false;  // start going left
    } else if (tach_rpm_angle <= 0) { // reached leftmost
        tach_rpm_angle_increasing = true;  // start going right
    }

    lv_color_t new_color = green_color;
    // new_color = (tach_rpm_angle > 75 ? red_color : green_color) ;
    // Only update color if changed
    lv_color_t old_color = lv_obj_get_style_arc_color(tach, LV_PART_INDICATOR);
    if (old_color.full != new_color.full) {
        lv_obj_set_style_arc_color(tach, new_color, LV_PART_INDICATOR);
    }
    lv_arc_set_value(tach, tach_rpm_angle);
}


void gauge_timer(lv_timer_t * t){
    simulate_tach_movement(ui_Arc1, 3);
    simulate_guage_movement(speed_mph, 180, 0, 1, ui_Label1, "speed_mph", speed_mph_increasing);
}

void app_main(void)
{
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false, // must be set to true for software rotation
        }
    };
    lv_display_t *disp = bsp_display_start_with_config(&cfg);

    // if (disp != NULL)
    // {
    //     bsp_display_rotate(disp, LV_DISPLAY_ROTATION_90); // 90、180、270
    // }

    bsp_display_backlight_on();

    bsp_display_lock(0);

    // lv_demo_music();
    // lv_demo_benchmark();
    // lv_demo_widgets();
    
    ui_init();

    init_label_styles();
    lv_timer_create(gauge_timer, 30, NULL);


    while(1){
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(30));
    }


    bsp_display_unlock();
}
