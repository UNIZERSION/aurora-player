#pragma once
#include "audio_engine.h"
#ifdef __cplusplus
extern "C" {
#endif

#define BT_BRIDGE_MAX_DEVICES 32
typedef struct {
    char mac[18],name[80];
    int16_t rssi;
    bool saved,selected;
} bt_bridge_device_t;
typedef enum { BT_REMOTE_NONE,BT_REMOTE_PLAY,BT_REMOTE_PAUSE,
               BT_REMOTE_NEXT,BT_REMOTE_PREVIOUS } bt_remote_action_t;
typedef struct {
    bool ready,enabled,local_output,awake,online,connected,scanning,scan_done;
    bool volume_pending,devices_ready; /* devices_ready follows a complete Pico LIST. */
    uint8_t volume,device_count;
    uint32_t revision,received_ms,pico_dropped,pico_empty,pico_buffered;
    uint32_t output_errors,remote_dropped;
    uint32_t stream_recoveries,pico_rx_blocks,pico_callbacks,pico_consumed,pico_recoveries,pico_input_stalls,pico_flushes;
    bool stream_failed;
    bool airpods_capable,airpods_connected,airpods_pending,airpods_connecting,airpods_auto_pause;
    int8_t airpods_mode,airpods_left,airpods_right,airpods_case,airpods_ears;
    uint32_t sbc_rate;
    int16_t sbc_channels,sbc_bitpool_min,sbc_bitpool_max;
    char airpods_message[112];
    char version[20],message[112];
    bt_bridge_device_t devices[BT_BRIDGE_MAX_DEVICES];
} bt_bridge_snapshot_t;

/* Init once before audio_engine_init. All returned sink callbacks are for the
 * audio output task. The local sink remains owned by the platform HAL. */
esp_err_t bluetooth_bridge_init(const audio_output_t *local,audio_output_t *routed);
void bluetooth_bridge_get_snapshot(bt_bridge_snapshot_t *out);
void bluetooth_bridge_set_enabled(bool enabled);
/* Applies codec mute immediately when idle, otherwise after the in-flight
 * write via the control worker. Preserves the engine's paused/muted state. */
void bluetooth_bridge_set_local_output(bool enabled);
esp_err_t bluetooth_bridge_set_volume(uint8_t percent);
/* Call regularly. Playback, the BT page and a connected receiver retain the
 * lease, including during pause so AVRCP PLAY can resume. With no receiver,
 * idle Bluetooth sleeps after 120 seconds; OFF/shutdown still sleep at once. */
void bluetooth_bridge_set_activity(bool playing,bool bluetooth_page);
/* Stop the audio engine first. Permanently quiesces this bridge until reboot:
 * discards commands/remote events, mutes local output, stops Pico I2S and waits
 * for STANDBY after SLEEP (or accepts an already confirmed deep sleep).
 * timeout_ms must be 1..60000; 2500 ms is recommended. A missing Pico returns
 * ESP_ERR_TIMEOUT; the shutdown latch remains set and prevents later wakeups.
 * This function must not run from the audio callbacks or the UART task. */
esp_err_t bluetooth_bridge_shutdown(uint32_t timeout_ms);
esp_err_t bluetooth_bridge_scan(void);
esp_err_t bluetooth_bridge_reconnect(void);
esp_err_t bluetooth_bridge_select(const char *mac);
esp_err_t bluetooth_bridge_test(void);
/* Controls are available only with a Pico advertising cap_airpods=1. States
 * reflect headphone acknowledgements; a queued request never changes mode. */
typedef enum {BT_AIRPODS_CONNECT,BT_AIRPODS_MODE,BT_AIRPODS_AUTO_PAUSE} bt_airpods_command_t;
esp_err_t bluetooth_bridge_airpods(bt_airpods_command_t command,int value);
/* Restore labels for display before Pico answers. Pairing keys stay on Pico;
 * root may call select only after a matching saved device is confirmed. */
void bluetooth_bridge_restore_devices(const bt_bridge_device_t *devices,size_t count);
/* Removes stale (>2 seconds) or disconnected events; caller handles playlist
 * availability and performs the same action as the on-screen controls. */
bt_remote_action_t bluetooth_bridge_pop_remote(void);

#ifdef __cplusplus
}
#endif
