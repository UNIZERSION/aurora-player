#pragma once
#include "esp_flash.h"
enum { ESP_PARTITION_TYPE_DATA = 1, ESP_PARTITION_SUBTYPE_DATA_NVS = 2, ESP_PARTITION_TYPE_ANY = 255, ESP_PARTITION_SUBTYPE_ANY = 255 };
struct esp_partition_t {
    esp_flash_t *flash_chip = esp_flash_default_chip;
    int type = ESP_PARTITION_TYPE_DATA, subtype = ESP_PARTITION_SUBTYPE_DATA_NVS;
    uint32_t address = 0xF10000, size = 0x6000;
    bool encrypted = false, readonly = false;
};
const esp_partition_t *esp_partition_find_first(int, int, const char *);
esp_err_t esp_partition_register_external(esp_flash_t *, size_t, size_t, const char *, int, int, const esp_partition_t **);
