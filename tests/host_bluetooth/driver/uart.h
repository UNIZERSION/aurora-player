#pragma once
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include <stddef.h>
typedef int uart_port_t;
enum {UART_NUM_2=2,UART_PIN_NO_CHANGE=-1,UART_DATA_8_BITS=8,UART_PARITY_DISABLE=0,UART_STOP_BITS_1=1,UART_HW_FLOWCTRL_DISABLE=0,UART_SCLK_DEFAULT=0};
struct uart_config_t {int baud_rate,data_bits,parity,stop_bits,flow_ctrl,source_clk;};
int uart_write_bytes(uart_port_t,const void*,size_t);
int uart_read_bytes(uart_port_t,void*,uint32_t,TickType_t);
esp_err_t uart_wait_tx_done(uart_port_t,TickType_t);
esp_err_t uart_flush_input(uart_port_t);
esp_err_t uart_set_pin(uart_port_t,int,int,int,int);
esp_err_t uart_param_config(uart_port_t,const uart_config_t*);
esp_err_t uart_driver_install(uart_port_t,int,int,int,void*,int);
esp_err_t uart_driver_delete(uart_port_t);
