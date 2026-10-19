#pragma once
#include "esp_err.h"
#include "tab5_keyboard_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Optional A164 Tab5 Keyboard on Ext.Port1 (SDA0, SCL1, IRQ50, address0x6D).
 * Starts an independent worker and I2C controller; an absent keyboard is normal.
 * Call once after BSP initialization, outside the LVGL lock. No USB/BLE HID. */
esp_err_t tab5_keyboard_init(void);
/* Nonblocking. Safe to call while the LVGL lock is held. Events are key presses,
 * including marked repeats; errors/unplug discard pending input and held keys. */
bool tab5_keyboard_read_event(tab5_keyboard_event_t *event);
bool tab5_keyboard_connected(void);

#ifdef __cplusplus
}
#endif
