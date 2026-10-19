#pragma once
#include "tap_gesture.h"
#include <cstdint>
namespace player {
// Single controller caller; worker performs no display, PIN, audio or SD work.
bool sleep_tap_start();
void sleep_tap_arm(bool enabled);
// Whether music is playing: a light series then does not count (see
// TapGestureDetector::set_playing). Cheap; call on every controller poll.
void sleep_tap_set_playing(bool playing);
TapGesture sleep_tap_take(uint32_t now_ms);
// Acknowledges that the worker has finished its last I2C operation and will
// perform no more. Optional sensor power-down failure must not prevent the
// device's physical power-off; no IMU interrupt is mapped to its wake/latch.
bool sleep_tap_stop_and_wait(uint32_t timeout_ms);
}
