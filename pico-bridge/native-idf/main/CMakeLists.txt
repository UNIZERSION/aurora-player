get_filename_component(PICO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
idf_component_register(SRCS
  "compat.cpp" "airpods.cpp" "${PICO_ROOT}/src/main.cpp"
  "${PICO_ROOT}/lib/ESP32-A2DP/src/BluetoothA2DPSource.cpp"
  "${PICO_ROOT}/lib/ESP32-A2DP/src/BluetoothA2DPCommon.cpp"
  INCLUDE_DIRS "." "${PICO_ROOT}/src" "${PICO_ROOT}/lib/ESP32-A2DP/src"
  REQUIRES bt nvs_flash driver esp_timer esp_system esp_driver_uart esp_driver_gpio)
target_compile_definitions(${COMPONENT_LIB} PRIVATE PICO_NATIVE_IDF=1
  A2DP_LEGACY_I2S_SUPPORT=0 A2DP_I2S_AUDIOTOOLS=0 A2DP_SPP_SUPPORT=0)
# src/ and lib/ sit outside this ESP-IDF project, so CONFIG_COMPILER_HIDE_PATHS_MACROS does not
# cover them: map their absolute path to "." so __FILE__ never embeds a local build path.
target_compile_options(${COMPONENT_LIB} PRIVATE -include time.h -Wno-error=deprecated-declarations
  "-fmacro-prefix-map=${PICO_ROOT}=.")
