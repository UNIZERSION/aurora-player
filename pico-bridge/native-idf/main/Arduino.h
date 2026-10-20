#pragma once
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_attr.h"
using String = std::string;
constexpr int SERIAL_8N1 = 0;
inline uint32_t millis() { return uint32_t(esp_timer_get_time()/1000); }
inline void delay(unsigned ms) { vTaskDelay(pdMS_TO_TICKS(ms ? ms : 1)); }
class Print {
 public:
  explicit Print(uart_port_t port): port_(port) {}
  void println(const char* value) { printf("%s\n", value); }
  int printf(const char* format, ...) {
    char text[1024]; va_list args; va_start(args,format);
    int n=vsnprintf(text,sizeof(text),format,args); va_end(args);
    if(n>0) uart_write_bytes(port_,text,std::min<size_t>(size_t(n),sizeof(text)-1));
    return n;
  }
  void flush() { uart_wait_tx_done(port_,pdMS_TO_TICKS(100)); }
 protected: uart_port_t port_;
};
class HardwareSerial: public Print {
 public:
  explicit HardwareSerial(uart_port_t port): Print(port) {}
  void begin(int baud, int=0, int rx=-1, int tx=-1) {
    uart_config_t config{};config.baud_rate=baud;config.data_bits=UART_DATA_8_BITS;
    config.parity=UART_PARITY_DISABLE;config.stop_bits=UART_STOP_BITS_1;
    config.flow_ctrl=UART_HW_FLOWCTRL_DISABLE;config.source_clk=UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_param_config(port_,&config));
    ESP_ERROR_CHECK(uart_set_pin(port_,tx,rx,UART_PIN_NO_CHANGE,UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(port_,2048,2048,0,nullptr,0));
  }
  void end() { uart_driver_delete(port_); }
  int available() { size_t n=0;uart_get_buffered_data_len(port_,&n);return int(n); }
  int read() { uint8_t byte=0;return uart_read_bytes(port_,&byte,1,0)==1 ? byte : -1; }
};
struct ESPClass {
  void restart() { esp_restart(); }
  uint32_t getFreeHeap() { return esp_get_free_heap_size(); }
};
extern HardwareSerial Serial,Serial2;
extern ESPClass ESP;
