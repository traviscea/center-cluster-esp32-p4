#pragma once
#include "lvgl.h"

extern lv_obj_t *boot_screen;

void boot_screen_create(void);
void boot_start(void);
void boot_finish(lv_obj_t *next_screen);