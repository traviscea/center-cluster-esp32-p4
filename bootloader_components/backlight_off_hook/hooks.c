// Runs at the very start of the second-stage bootloader, a fraction of a
// second after power-on. Until something drives it, the backlight pin is left
// floating and the backlight comes on by itself; this drives it to "off" right
// away. The app's backlight driver (bsp_display_brightness_init) takes the pin
// over later, still off, and the startup sequence turns it on.

#include <stdbool.h>
#include "soc/io_mux_reg.h"
#include "soc/gpio_sig_map.h"
#include "hal/gpio_ll.h"
#include "esp_rom_gpio.h"

// BSP_LCD_BACKLIGHT on the Waveshare ESP32-P4-WIFI6-Touch-LCD board. The
// backlight is active-low (the BSP inverts its PWM), so high = off.
#define BACKLIGHT_GPIO 26

void bootloader_hooks_include(void)
{
}

void bootloader_before_init(void)
{
    gpio_ll_func_sel(&GPIO, BACKLIGHT_GPIO, PIN_FUNC_GPIO);
    esp_rom_gpio_connect_out_signal(BACKLIGHT_GPIO, SIG_GPIO_OUT_IDX, false, false);
    gpio_ll_set_level(&GPIO, BACKLIGHT_GPIO, 1);
    gpio_ll_output_enable(&GPIO, BACKLIGHT_GPIO);
}
