# Bootloader hook: switches the LCD backlight off as early as possible at
# power-on, so the center screen doesn't glow before the startup sequence.
idf_component_register(SRCS "hooks.c"
                       PRIV_REQUIRES hal soc esp_rom)

# The bootloader hooks are weak symbols, so the linker would drop this library
# unless something references it: bootloader_hooks_include() is that reference.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u bootloader_hooks_include")
