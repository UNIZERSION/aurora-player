idf_component_register(SRCS "tab5_hal.c" "tab5_display.c" "tab5_power.c" "tab5_accessories.c" "tab5_keyboard.c" "tab5_keyboard_protocol.c" "tab5_motion.c" INCLUDE_DIRS "include"
    REQUIRES espressif__m5stack_tab5 lvgl fatfs esp_driver_i2c esp_driver_sdmmc
    PRIV_REQUIRES esp_timer esp_driver_ppa esp_mm espressif__esp_lcd_touch espressif__bmi270)
