file(GLOB DOOM_SRCS "doomgeneric/*.c")

idf_component_register(
    SRCS 
        "main.cpp"
        "SdUsbManager.cpp"
        ${DOOM_SRCS}
    INCLUDE_DIRS 
        "."
        "doomgeneric"
    REQUIRES 
        esp_lcd 
        lvgl 
        esp_timer 
        driver 
        nvs_flash 
        esp_wifi 
        app_update 
        bsp_board_extra
    LDFRAGMENTS 
        "linker.lf"
)

set_source_files_properties(${DOOM_SRCS} PROPERTIES COMPILE_FLAGS "-w")