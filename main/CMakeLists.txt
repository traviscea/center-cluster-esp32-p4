idf_component_register(
    SRCS 
        "main.c"
        "tach_ui/ui.c"
        "tach_ui/screens/ui_Screen1.c"
        "tach_ui/images/ui_img_1656279599.c"
        "tach_ui/fonts/ui_font_DotoLarge.c"
        "tach_ui/fonts/school_bell_48.c"
        "tach_ui/images/ui_img_sti_logo_png.c"
        "tach_ui/boot/boot_screen.c"

    INCLUDE_DIRS 
        "./tach_ui"
        "."
    
)

idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
target_compile_options(
    ${LVGL_LIB} 
    PRIVATE 
        -DLV_LVGL_H_INCLUDE_SIMPLE
        -DLV_USE_DEMO_MUSIC
)
