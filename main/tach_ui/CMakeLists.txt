SET(SOURCES screens/ui_Screen1.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    images/ui_img_1656279599.c
    images/ui_img_sti_logo_png.c
    fonts/ui_font_dial_74.c
    fonts/ui_font_dial_30.c
    fonts/ui_font_speed_156.c
    fonts/ui_font_gear_136.c
    fonts/ui_font_odo_42.c)

add_library(ui ${SOURCES})
