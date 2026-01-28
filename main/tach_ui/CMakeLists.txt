SET(SOURCES screens/ui_Screen1.c
    ui.c
    components/ui_comp_hook.c
    ui_helpers.c
    images/ui_img_1656279599.c
    images/ui_img_sti_logo_png.c
    fonts/ui_font_DotoLarge.c
    fonts/school_bell_48.c)

add_library(ui ${SOURCES})
