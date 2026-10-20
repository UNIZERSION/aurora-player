#include "Arduino.h"
#include "nvs_flash.h"
HardwareSerial Serial(UART_NUM_0),Serial2(UART_NUM_2);
ESPClass ESP;
void setup();
void loop();
extern "C" void app_main() {
  // Keep existing pairings. Never erase NVS as an automatic recovery.
  ESP_ERROR_CHECK(nvs_flash_init());
  setup();
  for(;;) loop();
}
