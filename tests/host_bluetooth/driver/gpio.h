#pragma once
#include "esp_err.h"
typedef int gpio_num_t;
enum {GPIO_NUM_5=5,GPIO_NUM_6=6,GPIO_NUM_18=18,GPIO_NUM_19=19,GPIO_NUM_46=46,GPIO_MODE_OUTPUT=1};
esp_err_t gpio_set_level(gpio_num_t,int);
esp_err_t gpio_reset_pin(gpio_num_t);
esp_err_t gpio_set_direction(gpio_num_t,int);
