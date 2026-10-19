idf_component_register(
    SRCS "main.cpp" "tap_gesture.cpp" "sleep_tap_input.cpp" "storage_layout.cpp" "screen_lock.cpp" "screen_lock_storage.cpp" "lock_wallpaper.cpp" "player_store.cpp" "favorites_store.cpp" "music_library.cpp" "player_ui.cpp" "ui_orbitron_28.c" "aurora_fonts.c"
         "ui_fonts.cpp" "ui_montserrat_latin_14.c" "ui_montserrat_latin_16.c" "ui_montserrat_latin_18.c"
         "ui_montserrat_latin_20.c" "ui_montserrat_latin_24.c" "ui_montserrat_latin_32.c"
         "audio_engine.c" "audio_file.c" "audio_fx.c" "audio_eq.c" "eq_store.cpp" "reverb_store.cpp" "audio_extra_codec.c" "bluetooth_bridge.cpp" "player_artwork.cpp" "player_artwork_decode.cpp" "vendor/tjpgd/tjpgd.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash spi_flash mbedtls tab5_hal esp_timer fatfs esp_app_format esp_driver_uart esp_driver_i2s esp_driver_gpio esp_driver_usb_serial_jtag esp_audio_codec
    EMBED_FILES "ui_shell.pack" "ui_dragon_thumb.rgb565" "aurora_controls.pack" "aurora_dragon.pack"
)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
# Enclosure tap controls: on since 1.3.21 (detector v2 on the BMI270 FIFO,
# docs/TAP-CONTROLS.md); -DAURORA_TAP_CONTROLS=0 builds without them.
# Calibration-only build: idf.py -B build-calib -DAURORA_TAP_STREAM=1 build
if(NOT DEFINED AURORA_TAP_CONTROLS)
    set(AURORA_TAP_CONTROLS 1)
endif()
if(AURORA_TAP_STREAM)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AURORA_TAP_STREAM=1 AURORA_TAP_CONTROLS=1)
elseif(AURORA_TAP_CONTROLS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AURORA_TAP_CONTROLS=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE AURORA_TAP_CONTROLS=0)
endif()
