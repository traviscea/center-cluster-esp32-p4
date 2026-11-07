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
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "driver/adc.h"
#include "driver/pulse_cnt.h"
#include "esp_timer.h"
#include "gps_wrapper.h"
#include "odometer/odometer.h"
#include "lap_timer.h"
#include "canbus.h"


// =======================================================
// SENSOR SOURCE CONFIG
// =======================================================
// for switching between analog or canbus reading
#define SENSOR_SOURCE_ANALOG 0
#define SENSOR_SOURCE_CAN    1

#define SENSOR_SOURCE SENSOR_SOURCE_ANALOG
// =======================================================
//-----Pin Assignment---------//

//GPS RX - GPIO 35
#define GPS_RX_PIN        35
#define GPS_UART_NUM      UART_NUM_3
#define GPS_TX_PIN        UART_PIN_NO_CHANGE  

//UART0 Transaction - GPIO 37
#define UART_TX_PIN 37

//UART1 Transaction - GPIO30
#define UART1_TX_PIN 30

//Water Temp - GPIO 20
#define WATER_TEMP_ADC_CHANNEL ADC1_CHANNEL_4

//Oil Temp - GPIO 50
#define OIL_TEMP_ADC_CHANNEL ADC2_CHANNEL_1

// Boost - GPIO 52
#define BOOST_ADC_CHANNEL ADC2_CHANNEL_3

//Oil Pressure - GPIO 21
#define OIL_PRESSURE_ADC_CHANNEL ADC1_CHANNEL_5

//Fuel Pressure - GPIO 22
#define FUEL_PRESSURE_ADC_CHANNEL ADC1_CHANNEL_6

#define TACH_GPIO GPIO_NUM_5

// AFR - GPIO 49
#define AFR_ADC_CHANNEL ADC2_CHANNEL_0 

// Fuel level - GPIO 51
#define FUEL_ADC_CHANNEL ADC2_CHANNEL_2

//--------------------------//


#define UART_PORT UART_NUM_1
#define UART1_PORT UART_NUM_2
#define GAUGE_PKT_SOF   0xA5
#define GAUGE_PKT_LEN   26
#define UART_TX_BUF_SIZE 256
#define UART_BAUD_RATE 2000000

#define ENABLE_LOGS false
#define ADC_UPDATE_PERIOD_MS 10
#define FILTER_SAMPLES_DEFAULT 8


//------------GPS-----------//         
#define GPS_BAUD_RATE     115200
#define GPS_BUF_SIZE          1024
#define GPS_MIN_VALID_MPH 3.0f

static char gps_speed_str[8];
static volatile float g_speed_mph = 0.0f;
static lv_obj_t *speed_label;
//--------------------------//

//--------UPDATE/REFRESH_DELAYS------//
#define FUEL_UPDATE_PERIOD 1000 // 1 updates a second
#define TEMP_UPDATE_DELAY 250 // 4 updates a second
#define PRESSURE_UPDATE_DELAY 50 // 20 updates a second
#define AFR_UPDATE_DELAY   20 // 50 updates a second
#define TACH_UPDATE_DELAY   7 // ~143 updates a second
//-----------------------------------//

//-------------TEMP-------------//
#define R_PULLUP       1000.0f
#define R1 10000.0f
#define R2 20000.0f
#define ADC_SCALE (R2 / (R1 + R2))
#define ADC_MAX        4095.0f
#define ADC_VREF       3.7f
#define SENSOR_SUPPLY  5.0f
#define ADC_WIDTH ADC_WIDTH_BIT_12
#define ADC_ATTEN ADC_ATTEN_DB_11

#define TEMP_SENSOR_TABLE_SIZE 9
const float tempF[] = {
    32, 68, 104, 140, 176, 212, 248, 284, 302
};

const float sensorR[] = {
    12000, 8000, 5830, 3020, 1670, 975, 599, 386, 316
};
//------------------------------//

//-------------BOOST-------------//
#define BOOST_FILTER_ALPHA 0.18F
#define BOOST_DIVIDER_SCALE (20.0f / (10.0f + 20.0f))
//equals voltage that boost sensor reads 0psi
#define BOOST_ZERO_OFFSET 1.0F
//adds psi for gauge calibration as all gauges read a little off...
#define BOOST_OFFSET 3.2F
//--------------------------------//


//-------------PRESSURE-------------//
#define ADC_ATTEN ADC_ATTEN_DB_11           
#define ADC_UNIT ADC_UNIT_1
#define OIL_FUEL_DIVIDER_SCALE ((10.0f + 20.0f) / 20.0f)

#define PRESSURE_SENSOR_TABLE_SIZE 5
const float Vpts[PRESSURE_SENSOR_TABLE_SIZE] = {0.5f, 1.3f, 2.5f, 3.7f, 4.5f};
const float PSIpts[PRESSURE_SENSOR_TABLE_SIZE] = {0.0f, 29.0f, 72.5f, 116.0f, 145.0f};
const float oil_fuel_pressure_alpha = 0.18f;
//------------------------------//


//-------------WIDEBAND AFR-------------//

#define AFR_DIVIDER_GAIN ((68.0f + 33.0f) / 33.0f)   // ≈ 3.06
#define AFR_OFFSET 0.7f 
#define AFR_FILTER_ALPHA 0.3f   // smoothing
//--------------------------------------//

//-------------RPM-------------//
#define MAX_PERIOD_CHANGE 0.12f   // 15% allowed change
#define PULSES_PER_REV 2
#define MIN_PULSE_COUNT 2       // minimum pulses before computing RPM
#define MAX_RPM 9000.0f
#define ARC_SCALE (100.0f / MAX_RPM) 
#define RPM_MIN_PERIOD  3000
#define RPM_TIMEOUT_MS  500    // If no pulse for this long, RPM = 0

static portMUX_TYPE tachMux = portMUX_INITIALIZER_UNLOCKED;
static pcnt_unit_handle_t pcnt_unit;
static int last_count = 0;
static int64_t last_time = 0;
static volatile float current_rpm = 0.0f;
volatile uint64_t lastTachUs = 0;
volatile uint64_t tachPeriodUs = 0;
volatile uint64_t lastPulseMs = 0;

float rpmLastRaw = 0.0f;
float rpmNow = 0.0f;
float rpmFiltered = 0.0f;

#define PERIOD_AVG_SAMPLES 4

uint64_t periodBuffer[PERIOD_AVG_SAMPLES] = {0};
int periodIndex = 0;

//------------------------------//

//-------------FUEL-------------//
#define FUEL_PULLUP_VOLTAGE   3.3f
#define ADC_MAX               4095.0f
#define PULLUP_RESISTOR_OHMS  150.0f
#define FILTER_SAMPLES_FUEL   16
#define BOOT_SETTLE_MS        1500

static uint8_t fuel_percent = 0;
static float fuel_ohms = 0.0f;
static int64_t boot_time_ms = 0;
//------------------------------//

// -------- GEAR DETECTION ------//

#define IDLE_RPM_THRESHOLD      1200.0f
#define NEUTRAL_RATIO_FACTOR    0.65f
#define GEAR_RATIO_TOL          14.0f
#define GEAR_CONFIRM_TIME       0.30f
#define GEAR_FILTER_ALPHA       0.18f
#define GEAR_SHIFT_LOCK_TIME    0.25f
#define GEAR_SLIP_RPM_RATE      2200.0f


/**
This is setup for a JDM STi 6spd
For your use case you would need to calculate a value for each gear using

    RPM_per_MPH = (GearRatio × FinalDrive × 336) / TireDiameter 

and add that to each below in the table.
**/
static const float gear_table[6] = {
    193.0f,  // 1
    126.0f,  // 2
    93.0f,   // 3
    71.0f,   // 4
    56.0f,   // 5
    44.0f    // 6
};

static float gear_filtered_ratio = 0;
static float gear_last_rpm = 0;
static float gear_last_speed = 0;

static int gear_confirmed = 0;
static int gear_candidate = 0;

static float gear_confirm_timer = 0;
static float gear_lock_timer = 0;

//------------------------------//

//-------------LOGGING------------//
static const char *TAG_TEMP = "TEMP_SENSOR";
static const char *TAG_PRESSURE = "PRESSURE_SENSOR";
static const char *TAG_TACH = "TACH_SENSOR";
static const char *TAG_FUEL = "FUEL_SENSOR";
static const char *TAG_GPS = "GPS_SENSOR";
static const char *TAG_AFR = "AFR_SENSOR";
//--------------------------------//

//------------DATA_SENT_OUT---------//
typedef struct {
    float oil_temp_f;
    float water_temp_f;
    float oil_pressure_psi;
    float fuel_pressure_psi;
    float fuel_level_pct;
    float afr;
    float boost_psi;
    float fuel_comp;
} gauge_data_t;

static gauge_data_t g_gauge_data;


typedef struct __attribute__((packed)) {
    uint16_t oil_temp;       // °F x10
    uint16_t water_temp;     // °F x10
    uint16_t oil_pressure;   // psi x10
    uint16_t fuel_pressure;  // psi x10
    uint16_t fuel_level;     // % x10
    uint16_t afr;            // AFR x10
    int16_t boost;           // psi x10
    uint32_t lap_time_ms;    // current lap in milliseconds
    int32_t  lap_delta_ms;   // predictive delta in ms
} gauge_payload_t;


//---------------------------------------//

static lv_color_t green_color;
static lv_color_t red_color;
static lv_color_t orange_color;
static lv_color_t purple_color;
static lv_color_t pink_color;
static lv_color_t blue_color;
static lv_color_t dial_color;
static lv_color_t dial_dim_color;

static inline int constrain_int(int x, int low, int high) {
    if (x < low) return low;
    if (x > high) return high;
    return x;
}

static void init_label_styles(void){

    green_color = lv_color_hex(0x28FF00);
    red_color = lv_palette_main(LV_PALETTE_RED);
    orange_color = lv_palette_main(LV_PALETTE_ORANGE);
    purple_color = lv_palette_main(LV_PALETTE_PURPLE);
    pink_color = lv_palette_main(LV_PALETTE_PINK);
    blue_color = lv_palette_main(LV_PALETTE_CYAN);
    dial_color = lv_color_hex(UI_COLOR_DIAL);
    dial_dim_color = lv_color_hex(UI_COLOR_DIAL_DIM);
}

static void update_label_if_needed(lv_obj_t *label, char *new_value, lv_color_t new_color) { 
    // Only update text if changed 
    const char *old_text = lv_label_get_text(label); 
    if (strcmp(old_text, new_value) != 0) { 
        lv_label_set_text(label, new_value); 
    } 
    // Only update color if changed 
    lv_color_t old_color = lv_obj_get_style_text_color(label, LV_PART_MAIN); 
    if (old_color.full != new_color.full) { 
        lv_obj_set_style_text_color(label, new_color, LV_PART_MAIN); 
    } 
}

static uint8_t clamp_u8(int val) {
    if (val < 0) return 0;
    if (val > 100) return 100;
    return (uint8_t)val;
}

//-----------------------FUEL---------------------------//



float fuel_pct_from_voltage(float v){
    const float V_FULL  = 0.06f;
    const float V_HALF  = 0.53f;
    const float V_25    = 0.845f;
    const float V_5     = 0.91f;
    const float V_EMPTY = 0.95f;

    if (v <= V_FULL)  return 100.0f;
    if (v >= V_EMPTY) return 0.0f;

    if (v <= V_HALF)
        return 50.0f +
               50.0f * (V_HALF - v) / (V_HALF - V_FULL);

    if (v <= V_25)
        return 25.0f +
               25.0f * (V_25 - v) / (V_25 - V_HALF);

    if (v <= V_5)
        return 5.0f +
               20.0f * (V_5 - v) / (V_5 - V_25);

    return 5.0f *
           (V_EMPTY - v) / (V_EMPTY - V_5);
}

//-----------------------------------------------------//



//-----------------------TEMP--------------------------//

float read_temp_resistance(int raw){

    float adc_voltage = (raw / ADC_MAX) * ADC_VREF;

    // Undo scaling divider
    float signal_voltage = adc_voltage / ADC_SCALE;

    // Calculate sensor resistance
    float sensor_resistance = R_PULLUP *
        (signal_voltage / (SENSOR_SUPPLY - signal_voltage));

    return sensor_resistance;
}

//-----------------------------------------------------//

//-----------------------_RPM---------------------------//

// Optional adaptive alpha for EMA
static inline float alphaForRPM(float rpmRaw) {
    // Base alpha depending on RPM range (smoother at low RPM, faster at high RPM)
    float base;
    if (rpmRaw < 1200.0f) base = 0.04f;      // idle, very smooth
    else if (rpmRaw < 3000.0f) base = 0.12f; // mid RPM
    else base = 0.20f;                      // high RPM, faster updates
    return base;                       
}

void IRAM_ATTR tachISR(void* arg) {
    uint64_t now = esp_timer_get_time();
    uint64_t dt = now - lastTachUs;
    if (dt < RPM_MIN_PERIOD) return;
    portENTER_CRITICAL_ISR(&tachMux);
    periodBuffer[periodIndex] = dt;
    periodIndex = (periodIndex + 1) % PERIOD_AVG_SAMPLES;
    tachPeriodUs = dt;   // store last good period
    lastTachUs = now;
    lastPulseMs = now / 1000;
    portEXIT_CRITICAL_ISR(&tachMux);
}

void tach_init() {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_POSEDGE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << TACH_GPIO);
    io_conf.pull_up_en = 0;
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(TACH_GPIO, tachISR, NULL);
}

void tach_task(void *arg) {
    const TickType_t delay = pdMS_TO_TICKS(TACH_UPDATE_DELAY);

    static int rejectStreak = 0;

    while (true) {
        float rpmRaw = 0.0f;
        int count = 0;

        float lastValid = (rpmLastRaw > 500.0f) ? rpmLastRaw :
                          (rpmFiltered > 500.0f ? rpmFiltered : 1000.0f);

        portENTER_CRITICAL(&tachMux);

        for (int i = 0; i < PERIOD_AVG_SAMPLES; i++) {

            if (periodBuffer[i] > 0) {

                float rpm =
                    60000000.0f /
                    (periodBuffer[i] * PULSES_PER_REV);

                float tolerance;

                if (lastValid < 1500.0f)
                    tolerance = 0.40f;
                else if (lastValid < 4000.0f)
                    tolerance = 0.25f;
                else
                    tolerance = 0.18f;

                // If we've rejected too long, relax the filter
                if (rejectStreak > 40)
                    tolerance *= 2.0f;

                if (fabsf(rpm - lastValid) < lastValid * tolerance) {
                    rpmRaw += rpm;
                    count++;
                }
            }
        }

        portEXIT_CRITICAL(&tachMux);

        if (count > 0) {
            rpmRaw /= count;
            rejectStreak = 0;
        } else {
            rejectStreak++;
            rpmRaw = rpmFiltered;
        }

        rpmLastRaw = rpmRaw;

        uint64_t nowMs = esp_timer_get_time() / 1000;

        if (nowMs - lastPulseMs > RPM_TIMEOUT_MS) {
            rpmFiltered = 0.0f;
        }
        else {

            float alpha;

            if (rpmRaw < 1200.0f)       alpha = 0.05f;
            else if (rpmRaw < 3000.0f)  alpha = 0.12f;
            else if (rpmRaw < 6000.0f)  alpha = 0.20f;
            else                        alpha = 0.30f;

            if (rpmFiltered == 0.0f)
                rpmFiltered = rpmRaw;
            else
                rpmFiltered += alpha * (rpmRaw - rpmFiltered);
        }

        rpmNow = rpmFiltered;

        vTaskDelay(delay);
    }
}


static int detect_gear(float rpm, float mph, float dt)
{
    static int current_gear = -1;   // -1 = N
    static float filtered_ratio = 0;

    if (mph < 2.0f) {
        current_gear = -1;
        filtered_ratio = 0;
        return -1;
    }

    float safe_mph = fmaxf(mph, 1.0f);
    float ratio = rpm / safe_mph;

    // Smooth ratio (GPS stabilization)
    filtered_ratio += 0.2f * (ratio - filtered_ratio);

    // ---------- FORCE 1ST DURING LAUNCH ----------
    if (mph < 12.0f && filtered_ratio > 150.0f) {
        current_gear = 1;
        return 1;
    }

    // ---------- CLUTCH / REV MATCH ----------
    float rpm_rate = (rpm - gear_last_rpm) / dt;
    float speed_rate = fabsf(mph - gear_last_speed);

    gear_last_rpm = rpm;
    gear_last_speed = mph;

    if (fabsf(rpm_rate) > 2000.0f && speed_rate < 0.8f) {
        current_gear = -1;
        return -1;
    }

    // ---------- IDLE NEUTRAL ----------
    if (rpm < 1350.0f && mph > 2.0f) {
        current_gear = -1;
        return -1;
    }

    // ---------- NORMAL GEAR MATCH ----------
    float smallest_error = 9999.0f;
    int best = current_gear;

    for (int i = 0; i < 6; i++) {
        float err = fabsf(filtered_ratio - gear_table[i]);
        if (err < smallest_error) {
            smallest_error = err;
            best = i + 1;
        }
    }

    // Only switch if clearly closer than current gear
    if (smallest_error < 18.0f) {
        current_gear = best;
    }

    return current_gear;
}

void tach_set_rpm(int rpm){
    // Needle position + shift glow are handled by the dial UI
    ui_tach_set_rpm(rpm);
}


void gauge_timer(lv_timer_t * t) {

    static float displayRPM = 0.0f;
    displayRPM += 0.20f * (rpmNow - displayRPM);

    tach_set_rpm(displayRPM);

    double miles = odometer_get_miles();
    char odo_buf[16];
    snprintf(odo_buf, sizeof(odo_buf), "%06.1f", miles);
    update_label_if_needed(ui_label_odometer_value, odo_buf, dial_dim_color);


    // -------- GEAR DETECTION -------- //
    static int64_t last_us = 0;
    int64_t now_us = esp_timer_get_time();
    float dt = (last_us == 0) ? 0.01f :
               (now_us - last_us) / 1000000.0f;
    last_us = now_us;

    float speed_for_gear = g_speed_mph;

    if (speed_for_gear < GPS_MIN_VALID_MPH)
        speed_for_gear = 0.0f;

    int gear = detect_gear(rpmNow, speed_for_gear, dt);

    static int last_displayed = -1;

    if (gear != last_displayed) {

        if(gear == -1) {
            update_label_if_needed(ui_label_gear_value, "N", dial_color);
        } else {
            char buf[2];
            snprintf(buf, sizeof(buf), "%d", gear);
            update_label_if_needed(ui_label_gear_value, buf, dial_color);
        }

        last_displayed = gear;
    }

    if (SENSOR_SOURCE == SENSOR_SOURCE_CAN){
        float speed_mph = g_speed_mph;

        if (speed_mph < GPS_MIN_VALID_MPH)
            speed_mph = 0.0f;

        int speed = (int)speed_mph;
        static char buf[8];
        snprintf(buf, sizeof(buf), "%d", speed);
        lv_label_set_text(ui_label_mph_value, buf);

        char afr_buf[12];
        snprintf(afr_buf, sizeof(afr_buf), "%4.1f", g_gauge_data.afr);

        char boost_buf[12];
        snprintf(boost_buf, sizeof(boost_buf), "%4.1f", g_gauge_data.boost_psi);


        char fuel_comp_buf[12];
        snprintf(fuel_comp_buf, sizeof(fuel_comp_buf), "%4.1f", g_gauge_data.fuel_comp);
        //***Update afr and boost and fuel_comp labels here, 
        // example -> update_label_if_needed(ui_label_odometer_value, odo_buf, dial_dim_color);
        // update_label_if_needed();
        // update_label_if_needed();
    
    }


}

//------------------------------------------------------------------------//



float resistance_to_F(float R) {
    if (R >= sensorR[0]) return tempF[0];
    if (R <= sensorR[TEMP_SENSOR_TABLE_SIZE - 1]) return tempF[TEMP_SENSOR_TABLE_SIZE - 1];

    for (int i = 0; i < TEMP_SENSOR_TABLE_SIZE - 1; i++) {
        if (R <= sensorR[i] && R >= sensorR[i+1]) {
            float t = tempF[i] + (sensorR[i] - R) * (tempF[i+1] - tempF[i]) / (sensorR[i] - sensorR[i+1]);
            return t; 
        }
    }
    return tempF[0];
}

float voltage_to_psi(float v) {

    // Below first threshold → clamp to 0 PSI
    if (v <= Vpts[0]) return 0.0f;
    // Above last threshold → clamp to max
    if (v >= Vpts[4]) return PSIpts[4];

    // Find segment and linearly interpolate
    for (int i = 0; i < 4; i++) {
        if (v < Vpts[i+1]) {
            float t = (v - Vpts[i]) / (Vpts[i+1] - Vpts[i]);
            return PSIpts[i] + t * (PSIpts[i+1] - PSIpts[i]);
        }
    }
    return 0.0f;
}

static uint16_t crc16_ccitt(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFF;

    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}


//---------------------------------GPS------------------------//

static void speed_update_cb(void *arg){
    lv_obj_t *label = (lv_obj_t *)arg;
    static bool last_fix = true;
    static int last_speed = -1;

    bool has_fix = gps_has_fix();

    if (!has_fix) {
        if (last_fix) {
            lv_label_set_text(label, "--");
            last_fix = false;
        }
        return;
    }

    float speed_mph = g_speed_mph;

    if (speed_mph < GPS_MIN_VALID_MPH)
        speed_mph = 0.0f;

    int speed = (int)speed_mph;

    if (!last_fix || speed != last_speed) {
        static char buf[8];
        snprintf(buf, sizeof(buf), "%d", speed);
        lv_label_set_text(label, buf);

        last_speed = speed;
        last_fix = true;
    }
}

void save_miles_task(void *arg){
    while (1){
        odometer_periodic_save();
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void gps_task(void *arg) {
    uint8_t buf[128];
    static double last_lat = 0.0;
    static double last_lon = 0.0;
    static bool first_fix = true;

    while (true) {
        int len = uart_read_bytes(GPS_UART_NUM, buf, sizeof(buf), portMAX_DELAY);
        for (int i = 0; i < len; i++) {
            gps_encode_char(buf[i]);
        }

        bool has_fix = gps_has_fix();
        int  sats_used = gps_sats_used();
        float hdop     = gps_hdop();

        static int last_speed = -1;

        if (has_fix) {
            float new_speed = gps_get_speed_mph();
            g_speed_mph = new_speed;

            int speed_int = (int)new_speed;

            if (speed_int != last_speed) {
                lv_async_call(speed_update_cb, ui_label_mph_value);
                last_speed = speed_int;
            }
            if (sats_used >= 5 && hdop < 3.5f && gps_location_updated()){
                double current_lat = gps_get_lat();
                double current_lon = gps_get_lon();
                
                //----- Lap timer(comment out if not needed )------//
                lap_timer_update(current_lat, current_lon, new_speed, has_fix);
                //------------------------------------------------//

                if (first_fix) {
                    last_lat = current_lat;
                    last_lon = current_lon;
                    first_fix = false;
                    continue;
                }

                double meters = gps_distance_between(last_lat, last_lon, current_lat, current_lon);

                // Filter GPS jitter (very important)
                if (new_speed > GPS_MIN_VALID_MPH && meters > 0.05 && meters < 100.0) {
                    static double meter_accumulator = 0.0;

                    meter_accumulator += meters;

                    if (meter_accumulator >= 1.0) {
                        uint32_t whole = (uint32_t)meter_accumulator;
                        odometer_add_meters(whole);
                        meter_accumulator -= whole;
                    }
                }

                last_lat = current_lat;
                last_lon = current_lon;
            }


        } else {
            g_speed_mph = 0;                
            lv_async_call(speed_update_cb, ui_label_mph_value);
        }
        #if ENABLE_LOGS
            ESP_LOGI(TAG_GPS,
                    "GPS Fix: %s | Speed: %.0f MPH | Sats Used: %d | HDOP: %.1f",
                    has_fix ? "YES" : "NO",
                    g_speed_mph,
                    sats_used,
                    hdop);
        #endif
        vTaskDelay(1);
    }
}

//------------------------------------------------------------------------//


//------------------------------ADC_UART---------------------------------------//
static void adc_global_init(void) {
    adc1_config_width(ADC_WIDTH);

    // ADC1 channels
    adc1_config_channel_atten(WATER_TEMP_ADC_CHANNEL, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(OIL_PRESSURE_ADC_CHANNEL, ADC_ATTEN_DB_11);
    adc1_config_channel_atten(FUEL_PRESSURE_ADC_CHANNEL, ADC_ATTEN_DB_11);
    

    // ADC2 channels
    adc2_config_channel_atten(BOOST_ADC_CHANNEL, ADC_ATTEN_DB_11);
    adc2_config_channel_atten(OIL_TEMP_ADC_CHANNEL, ADC_ATTEN_DB_11);
    adc2_config_channel_atten(FUEL_ADC_CHANNEL, ADC_ATTEN_DB_11);
    adc2_config_channel_atten(AFR_ADC_CHANNEL, ADC_ATTEN_DB_11);

    ESP_LOGI("ADC", "ADC Global Init Complete");
}

uint32_t sample_sum_adc1(adc1_channel_t adc_channel, int samples){
    uint32_t sum = 0;
    for (int i = 0; i < samples; i++) {
        sum += adc1_get_raw(adc_channel);
    }
    return sum / samples;
}

uint32_t sample_sum_adc2(adc2_channel_t adc_channel, int samples){
    uint32_t sum = 0;
    int raw = 0;

    for (int i = 0; i < samples; i++) {
        adc2_get_raw(adc_channel, ADC_WIDTH, &raw);
        sum += raw;
    }

    return sum / samples;
}

static void adc_task(void *arg) {
    int64_t last_temp_ms     = 0;
    int64_t last_pressure_ms = 0;
    int64_t last_afr_ms      = 0;
    int64_t last_fuel_ms     = 0;
    int64_t last_tx_ms       = 0;
    boot_time_ms = esp_timer_get_time() / 1000;
    static float water_filtered = -1;
    static float oil_filtered = -1;
    static float oil_press_filtered = -1;
    static float fuel_press_filtered = -1;
    static float fuel_filtered = 0.0f;
    static bool fuel_initialized = false;
    static bool fuel_ever_valid = false;


    while (1) {
        int64_t now_ms = esp_timer_get_time() / 1000;

        // ---------- Temperature Update ---------- //
        if (now_ms - last_temp_ms >= TEMP_UPDATE_DELAY) {
            last_temp_ms = now_ms;

            // Water temp (ADC1)
            //int raw_water = adc1_get_raw(WATER_TEMP_ADC_CHANNEL);
            int raw_water = sample_sum_adc1(WATER_TEMP_ADC_CHANNEL, FILTER_SAMPLES_DEFAULT);
            float R_water = read_temp_resistance(raw_water);

            // Oil temp (ADC2)
            // adc2_get_raw(OIL_TEMP_ADC_CHANNEL, ADC_WIDTH, &raw_oil);
            int raw_oil = sample_sum_adc2(OIL_TEMP_ADC_CHANNEL, FILTER_SAMPLES_DEFAULT);
            float R_oil = read_temp_resistance(raw_oil);

            float water_new = resistance_to_F(R_water);
            float oil_new   = resistance_to_F(R_oil);

            if (water_filtered < 0) water_filtered = water_new;
            if (oil_filtered   < 0) oil_filtered   = oil_new;

            water_filtered = water_filtered * 0.95f + water_new * 0.05f;
            oil_filtered   = oil_filtered   * 0.95f + oil_new   * 0.05f;

            g_gauge_data.water_temp_f = water_filtered;
            g_gauge_data.oil_temp_f   = oil_filtered;
        }

        // ---------- Pressure Update ---------- //
        if (now_ms - last_pressure_ms >= PRESSURE_UPDATE_DELAY) {
            last_pressure_ms = now_ms;

            // Oil pressure (ADC1)
            //int raw_oil_press = adc1_get_raw(OIL_PRESSURE_ADC_CHANNEL);
            float raw_oil_press = sample_sum_adc1(OIL_PRESSURE_ADC_CHANNEL, FILTER_SAMPLES_DEFAULT);
            float voltage_oil = ((float)raw_oil_press / 4095.0f) * ADC_VREF;

            // Fuel pressure (ADC1)
            //int raw_fuel_press = adc1_get_raw(FUEL_PRESSURE_ADC_CHANNEL);
            int raw_fuel_press = sample_sum_adc1(FUEL_PRESSURE_ADC_CHANNEL, FILTER_SAMPLES_DEFAULT);
            float voltage_fuel = ((float)raw_fuel_press / 4095.0f) * ADC_VREF;
    

            float oil_press_new = voltage_to_psi(voltage_oil * OIL_FUEL_DIVIDER_SCALE);
            float fuel_press_new = voltage_to_psi(voltage_fuel * OIL_FUEL_DIVIDER_SCALE);

            if (oil_press_filtered < 0) oil_press_filtered = oil_press_new;
            if (fuel_press_filtered < 0) fuel_press_filtered = fuel_press_new;

            oil_press_filtered =
                oil_press_filtered + oil_fuel_pressure_alpha * (oil_press_new - oil_press_filtered);

            fuel_press_filtered =
                fuel_press_filtered + oil_fuel_pressure_alpha * (fuel_press_new - fuel_press_filtered);


            g_gauge_data.fuel_pressure_psi = fuel_press_filtered;
            g_gauge_data.oil_pressure_psi = oil_press_filtered;

            // ---------- Boost pressure (ADC2) ----------
            int raw_boost;
            adc2_get_raw(BOOST_ADC_CHANNEL, ADC_WIDTH, &raw_boost);
            float voltage_boost = ((float)raw_boost / 4095.0f) * ADC_VREF;
            // Undo any voltage divider if present
            float sensor_voltage = voltage_boost / BOOST_DIVIDER_SCALE;
            // Prosport sender scales ~1V @ 0 PSI to ~4V @ ~43.5 PSI
            float pressure_psi = (sensor_voltage - BOOST_ZERO_OFFSET) * 14.5f;
            pressure_psi += BOOST_OFFSET;
            // Clamp to realistic limits
            if (pressure_psi < -15.0f) pressure_psi = -15.0f;
            if (pressure_psi > 45.0f)  pressure_psi = 45.0f;
            //Simple EMA filter
            static float boost_filtered = 0.0f;
            boost_filtered = boost_filtered * (1 - BOOST_FILTER_ALPHA)
                            + pressure_psi * BOOST_FILTER_ALPHA;
            g_gauge_data.boost_psi = boost_filtered;

        }

        // ---------- Wideband AFR (ADC2) ---------- //
        if (now_ms - last_afr_ms >= AFR_UPDATE_DELAY) { 
            last_afr_ms = now_ms; 
            int raw_afr; 
            adc2_get_raw(AFR_ADC_CHANNEL, ADC_WIDTH, &raw_afr); 
            float adc_voltage = ((float)raw_afr / 4095.0f) * ADC_VREF; 
            // Undo voltage divider to get actual AEM output voltage 
            float wb_voltage = adc_voltage * AFR_DIVIDER_GAIN; 
            // Apply AEM linear scaling (Page 11) 
            float afr = (2.3750f * wb_voltage) + 7.3125f;
            afr += AFR_OFFSET;
            // Optional clamp for sanity 
            if (afr < 7.0f) afr = 7.0f; 
            if (afr > 22.0f) afr = 22.0f; 
            // Simple EMA filter 
            static float afr_filtered = 14.7f; 
            afr_filtered = afr_filtered + AFR_FILTER_ALPHA * (afr - afr_filtered); 
            g_gauge_data.afr = afr_filtered;
        }

        // ---------- Fuel Gauge Update ----------
        if (now_ms - last_fuel_ms >= FUEL_UPDATE_PERIOD) {
            last_fuel_ms = now_ms;

            float avg_raw = sample_sum_adc2(FUEL_ADC_CHANNEL, FILTER_SAMPLES_DEFAULT);
            float vFuel = (avg_raw * ADC_VREF) / ADC_MAX;

            bool fuelSettled =
                (now_ms - boot_time_ms) > BOOT_SETTLE_MS;

            bool fuelValid = fuelSettled && (vFuel <= 3.29f);

            if (fuelValid) {

                float new_pct = fuel_pct_from_voltage(vFuel);

                // Initialize once
                if (!fuel_initialized) {
                    fuel_filtered = new_pct;
                    fuel_initialized = true;
                }

                float alpha;

                if (new_pct < fuel_filtered) {
                    // Tank dropping → respond faster
                    alpha = 0.20f;   // 20% per second
                } else {
                    // Tank rising (slosh / incline) → respond slowly
                    alpha = 0.10f;   // 10% per second
                }

                fuel_filtered += alpha * (new_pct - fuel_filtered);

                fuel_percent = fuel_filtered; 
                g_gauge_data.fuel_level_pct = fuel_percent;

                fuel_ever_valid = true;

            } else {

                if (!fuel_ever_valid) {
                    fuel_percent = 0;
                    g_gauge_data.fuel_level_pct = 0;
                }

                ESP_LOGW(TAG_FUEL, "Fuel invalid V=%.3f", vFuel);
            }
        }
        if (now_ms - last_tx_ms >= 20) {
            last_tx_ms = now_ms;
            static uint8_t seq = 0;
            uint8_t buf[GAUGE_PKT_LEN];

            buf[0] = GAUGE_PKT_SOF;
            buf[1] = seq++;

            gauge_payload_t *p = (gauge_payload_t *)&buf[2];

            p->oil_temp      = (uint16_t)(g_gauge_data.oil_temp_f * 10.0f);
            p->water_temp    = (uint16_t)(g_gauge_data.water_temp_f * 10.0f);
            p->oil_pressure  = (uint16_t)(g_gauge_data.oil_pressure_psi * 10.0f);
            p->fuel_pressure = (uint16_t)(g_gauge_data.fuel_pressure_psi * 10.0f);
            p->fuel_level    = (uint16_t)(g_gauge_data.fuel_level_pct * 10.0f);
            p->afr           = (uint16_t)(g_gauge_data.afr * 10.0f);
            p->boost         = (int16_t)(g_gauge_data.boost_psi * 10.0f);
            
            uint64_t lap_us = lap_timer_get_current_us();
            int32_t  delta_us = lap_timer_get_delta_us();

            p->lap_time_ms  = (uint32_t)(lap_us / 1000);
            p->lap_delta_ms = (int32_t)(delta_us / 1000);

            uint16_t crc = crc16_ccitt(&buf[1], GAUGE_PKT_LEN - 3);
            memcpy(&buf[GAUGE_PKT_LEN - 2], &crc, 2);

            uart_write_bytes(UART_PORT, buf, GAUGE_PKT_LEN);
            uart_write_bytes(UART1_PORT, buf, GAUGE_PKT_LEN);
        }

        // Optional logging
        #if ENABLE_LOGS
            ESP_LOGI(TAG_FUEL,
                    "Fuel:  %%=%u",
                    fuel_percent);
            ESP_LOGI(TAG_TEMP,
                    "Water: %.1fF  Oil: %.1fF",
                    g_gauge_data.water_temp_f,
                    g_gauge_data.oil_temp_f);
            ESP_LOGI(TAG_PRESSURE,
                    "Oil PSI: %.2f  Fuel PSI: %.2f Boost PSI: %.2f ",
                    g_gauge_data.oil_pressure_psi,
                    g_gauge_data.fuel_pressure_psi,
                    g_gauge_data.boost_psi);
            ESP_LOGI(TAG_AFR,
                    "AFR: %.2f",
                    g_gauge_data.afr);
        #endif

        vTaskDelay(pdMS_TO_TICKS(ADC_UPDATE_PERIOD_MS));
    }
}


static void uart_init(uart_port_t uart_num, int txPin, int rxPin, int bufSize, int baud) {
    uart_config_t cfg = {
        .baud_rate = baud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT 
    };

    ESP_ERROR_CHECK(uart_driver_install(
        uart_num,
        bufSize,   // TX buffer
        0,         // RX buffer
        0,
        NULL,
        0
    ));

    ESP_ERROR_CHECK(uart_param_config(uart_num, &cfg));

    ESP_ERROR_CHECK(uart_set_pin(
        uart_num,
        txPin,
        rxPin,
        UART_PIN_NO_CHANGE,
        UART_PIN_NO_CHANGE
    ));
}

static void can_mapping_task(void *arg){
    int64_t last_tx_ms  = 0;
    int64_t last_odo_ms = 0;

    while (1){
        int64_t now_ms = esp_timer_get_time() / 1000;

        // ---------- Drivetrain ----------
        rpmNow = can_data.rpm;

        // CAN speed usually in KPH
        g_speed_mph = can_data.speed * 0.621371f;

        // ---------- Odometer ----------
        if (now_ms - last_odo_ms >= 1000){
            last_odo_ms = now_ms;

            float speed_kph = can_data.speed;

            // meters per second
            float meters_per_sec = speed_kph / 3.6f;

            uint32_t whole = (uint32_t)meters_per_sec;

            if (whole > 0)
                odometer_add_meters(whole);
        }

        // ---------- Gauge data ----------
        g_gauge_data.water_temp_f     = can_data.coolant_temp;
        g_gauge_data.oil_pressure_psi = can_data.oil_pressure;
        g_gauge_data.afr = can_data.air_fuel_ratio;
        g_gauge_data.boost_psi = can_data.boost;
        g_gauge_data.fuel_comp = can_data.fuel_comp;
        

        // ---------- UART TX ----------
        if (now_ms - last_tx_ms >= 20){
            last_tx_ms = now_ms;

            static uint8_t seq = 0;
            uint8_t buf[GAUGE_PKT_LEN];

            buf[0] = GAUGE_PKT_SOF;
            buf[1] = seq++;

            gauge_payload_t *p = (gauge_payload_t *)&buf[2];

            p->oil_temp      = (uint16_t)(g_gauge_data.oil_temp_f * 10.0f);
            p->water_temp    = (uint16_t)(g_gauge_data.water_temp_f * 10.0f);
            p->oil_pressure  = (uint16_t)(g_gauge_data.oil_pressure_psi * 10.0f);
            p->fuel_pressure = (uint16_t)(g_gauge_data.fuel_pressure_psi * 10.0f);
            p->fuel_level    = (uint16_t)(g_gauge_data.fuel_level_pct * 10.0f);
            p->afr           = (uint16_t)(g_gauge_data.afr * 10.0f);
            p->boost         = (int16_t)(g_gauge_data.boost_psi * 10.0f);

            uint64_t lap_us = lap_timer_get_current_us();
            int32_t  delta_us = lap_timer_get_delta_us();

            p->lap_time_ms  = (uint32_t)(lap_us / 1000);
            p->lap_delta_ms = (int32_t)(delta_us / 1000);

            uint16_t crc = crc16_ccitt(&buf[1], GAUGE_PKT_LEN - 3);
            memcpy(&buf[GAUGE_PKT_LEN - 2], &crc, 2);

            uart_write_bytes(UART_PORT, buf, GAUGE_PKT_LEN);
            uart_write_bytes(UART1_PORT, buf, GAUGE_PKT_LEN);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}


//------------------------------------------------------------------------//

void app_main(void) {
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
        }
    };
    lv_display_t *disp = bsp_display_start_with_config(&cfg);

    adc_global_init();
    init_label_styles();
    tach_init();
    odometer_init();

    // LVGL isn't thread-safe: hold the display lock while building the UI so
    // the LVGL port task can't render at the same time.
    bsp_display_lock(0);
    ui_init();
    lv_timer_create(gauge_timer, 10, NULL);
    bsp_display_unlock();

    uart_init(UART_PORT, UART_TX_PIN, UART_PIN_NO_CHANGE, UART_TX_BUF_SIZE, UART_BAUD_RATE); 
    uart_init(UART1_PORT, UART1_TX_PIN, UART_PIN_NO_CHANGE, UART_TX_BUF_SIZE, UART_BAUD_RATE); 
    uart_init(GPS_UART_NUM, UART_PIN_NO_CHANGE, GPS_RX_PIN, (GPS_BUF_SIZE*2), GPS_BAUD_RATE); 

    if (SENSOR_SOURCE == SENSOR_SOURCE_CAN){
        canbus_init();
        xTaskCreatePinnedToCore(canbus_task,"can_rx",4096,NULL,10,NULL,0);
        xTaskCreatePinnedToCore(can_mapping_task,"can_mapping_task",4096,NULL,10,NULL,1);
    } else {
        xTaskCreatePinnedToCore(tach_task, "tach_task", 4096, NULL, 10, NULL, 0);
        xTaskCreatePinnedToCore(gps_task, "gps_task", 4096, NULL, 5, NULL, 0);
        xTaskCreatePinnedToCore(adc_task, "adc_uart_task", 4096, NULL, 5, NULL, 0);
    }

    xTaskCreatePinnedToCore(save_miles_task, "save_miles_task", 4096, NULL, 4, NULL, 0);

    bsp_display_backlight_off();
    vTaskDelay(pdMS_TO_TICKS(100)); 
    bsp_display_brightness_set(70); 

}
