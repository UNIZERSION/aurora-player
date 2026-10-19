#pragma once
#include "nvs.h"
struct esp_flash_t {};
extern esp_flash_t *esp_flash_default_chip;
esp_err_t esp_flash_read(esp_flash_t *, void *, uint32_t, uint32_t);
