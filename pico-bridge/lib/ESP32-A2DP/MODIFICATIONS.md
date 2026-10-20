# ESP32-A2DP: local copy and modifications

This folder is a partial copy of [pschatzmann/ESP32-A2DP](https://github.com/pschatzmann/ESP32-A2DP)
version **1.8.11** (tag `v1.8.11`, commit `b6da4744286ca15b6f1dee607c077a4747294dd4`), licensed
under the Apache License 2.0 (see [LICENSE](LICENSE), copied unchanged from upstream). Upstream's
`docs/`, `examples/`, `Doxyfile`, `keywords.txt` and CI files are not included. Upstream's
`CMakeLists.txt` comment says "copyright GPLv3"; the upstream project and its file headers state
Apache-2.0, which is what applies here.

Only `src/BluetoothA2DPSource.cpp` and `src/BluetoothA2DPCommon.cpp` are compiled as sources into
the Pico firmware (see `../../native-idf/main/CMakeLists.txt`), together with the headers they
include. Every file not listed below is identical to upstream v1.8.11, so the copy can be
compared with upstream.

## Modified files

Changed for the Aurora Pico Bluetooth bridge (2026). Each modified file says so in its first line.

- `src/A2DPVolumeControl.h`: two `constexpr float` constants computed with `pow()` became
  `const float` (compiler compatibility).
- `src/BluetoothA2DPSource.h`: includes `<atomic>` and `SourceMediaState.h`; new
  `set_discovery_enabled()` (stops repeated inquiry while keeping the discovered list); new
  media API `set_media_active()`, `service_media()`, `recover_media()`, `is_media_active()`; new
  members for the media state machine.
- `src/BluetoothA2DPSource.cpp`:
  - media commands (CHECK / START / SUSPEND) are serialized on the Bluetooth task through a
    private media tick event and the `SourceMediaState` state machine; pause is a real A2DP
    SUSPEND and recovery runs SUSPEND, CHECK, START without disconnecting the headphones;
  - the SDK's `len == -1` flush request is passed through instead of being divided;
  - volume is applied only to the bytes actually returned by the data callback;
  - discovery can be switched off and the inquiry length is 5 instead of 10; the one-time wait
    after the Bluetooth stack starts, before the first reconnect or discovery, is 1.2 s instead
    of 10 s; the heartbeat (connection) timer, which paces later reconnect attempts, is 5 s
    instead of 10 s;
  - `handle_reconnect_logic()` checks the result of `connect_to()` and only on success sets the
    CONNECTING state and resets the connecting-heartbeat counter (upstream always returned
    true);
  - a `PICO_NATIVE_IDF` hook reports the negotiated SBC configuration to the firmware;
  - fixes: the AVRCP target event copy uses `sizeof(esp_avrc_tg_cb_param_t)`, and the inverted
    result check of `esp_avrc_tg_set_psth_cmd_filter()`; the passthrough filter is limited to
    PLAY, STOP, PAUSE, FORWARD and BACKWARD.

## Added file

- `src/SourceMediaState.h`: the A2DP media state machine used above. It is part of the Pico
  Bluetooth bridge, not of upstream ESP32-A2DP, and is covered by this repository's license
  (GPL-3.0-only, see the top-level [NOTICE.md](../../../NOTICE.md)).
