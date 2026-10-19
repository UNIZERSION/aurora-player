# AirPods controls

The AirPods settings page is an optional extension to the Bluetooth dialog. The main player layout stays unchanged. The entry is offered only when the connected Pico advertises AirPods-control capability. Stable Pico 0.9.3 does not advertise that capability; the experimental 0.9.4 candidate does. AirPods protocol and hardware support require separate validation; successful host UI tests do not prove that a particular headset supports a feature.

The page is a 940×560 modal with three equal battery cards, four equal listening-mode buttons, Auto pause, Connect, and Close. Modes are Off, Noise cancellation, Transparency, and Adaptive. Battery values are displayed only when the control channel is connected and the report is in the range 0–100; unknown, invalid, and disconnected values are `--`. Zero percent is a valid reading.

A request never changes the selected mode or confirmed Auto pause value optimistically. The previous mode remains selected while a command awaits confirmation. A timeout or error leaves that selection unchanged. A new reported mode changes the highlight. Mode and Auto pause controls remain disabled until the capability, headphone connection, and control channel are present, and are disabled while connecting or awaiting a command response. Connect is available for a selected, connected headset whose control channel is disconnected. Close always remains available. The event handler enforces these conditions independently of the visual disabled state.

Auto pause is an application preference reported by Pico. Aurora owns decisions to pause/resume based on ear reports. A manual playback action must cancel any pending automatic-resume ownership. The view only requests that preference; it never decides playback state itself.

## UI integration

`main/aurora_airpods.inc` is a self-contained LVGL view. Include it once in `main.cpp` beside `aurora_reverb.inc`. Build and refresh it only while holding the existing LVGL mutex:

```cpp
AirPodsDialogState view{};
view.capable = bt.airpods_capable;
view.headphones_connected = bt.connected;
view.channel_connected = bt.airpods_connected;
view.connecting = bt.airpods_connecting;
view.pending = bt.airpods_pending;
view.mode = bt.airpods_mode;
view.battery_left = bt.airpods_left;
view.battery_right = bt.airpods_right;
view.battery_case = bt.airpods_case;
view.ears = bt.airpods_ears;
view.auto_pause = bt.airpods_auto_pause;
view.auto_pause_known = bt.airpods_connected;
view.status = bt.airpods_message;
airpods_dialog_build(panel, view, callback); // once
airpods_dialog_refresh(panel, view);         // later confirmed snapshots
```

The status string is copied immediately into the label and is not retained by the view. `auto_pause_known` must be true only after an actual status report; every successful connected-channel report includes that preference. Unknown values retain their default `-1`/false-known state. The callback has signature `void(int action, int value)` and must enqueue controller work rather than perform Bluetooth I/O under the LVGL mutex:

| Action | Value | Controller operation |
|---|---|---|
| `AIRPODS_SET_MODE` | 1, 2, 3, 4 | `BT_AIRPODS_MODE`: Off, ANC, Transparency, Adaptive |
| `AIRPODS_SET_AUTOPAUSE` | 0 or 1 | `BT_AIRPODS_AUTO_PAUSE` |
| `AIRPODS_CONNECT` | 0 | `BT_AIRPODS_CONNECT` |
| `AIRPODS_CLOSE` | 0 | Close the modal |

Repeated identical snapshots update no styles or label text and cause no redraw. Deleting the modal releases its bindings and state through the LVGL deletion event. No background timers, transport handles, or permanent heap allocation belong to the view.

## Verification

`tests/airpods_ui_test.cpp` drives real LVGL pointer events and checks capability/channel gating, rejected forged events while disabled, confirmed-only mode and Auto pause selection, pending/error/disconnect behavior, valid zero and unknown battery readings, equal 207×64 mode targets, unchanged-snapshot zero redraw, full-redraw pixel consistency, and clean deletion. Its runner output and screenshots are not published. Physical Pico/AirPods testing remains separate.
