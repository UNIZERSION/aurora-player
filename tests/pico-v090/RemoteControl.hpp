#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace remote {
enum class Action : uint8_t { None, Play, Pause, Next, Previous };
// AVRCP operation IDs. Multi-press gestures are decoded by the headphones.
inline Action fromKey(uint8_t key) {
  switch (key) {
    case 0x44: return Action::Play;
    case 0x45: case 0x46: return Action::Pause;
    case 0x4B: return Action::Next;
    case 0x4C: return Action::Previous;
    default: return Action::None;
  }
}
inline const char* line(Action action) {
  switch (action) {
    case Action::Play: return "REMOTE PLAY";
    case Action::Pause: return "REMOTE PAUSE";
    case Action::Next: return "REMOTE NEXT";
    case Action::Previous: return "REMOTE PREVIOUS";
    default: return "";
  }
}
inline Action parse(const char* value) {
  if (!value) return Action::None;
  if (std::strcmp(value, "REMOTE PLAY") == 0) return Action::Play;
  if (std::strcmp(value, "REMOTE PAUSE") == 0) return Action::Pause;
  if (std::strcmp(value, "REMOTE NEXT") == 0) return Action::Next;
  if (std::strcmp(value, "REMOTE PREVIOUS") == 0) return Action::Previous;
  return Action::None;
}
// Caller holds its task lock. No allocation or UART output in BT callbacks.
class Inbox {
 public:
  static constexpr size_t capacity = 16;
  static constexpr uint32_t maxAgeMs = 2000;
  void connected(bool connected) {
    if (connected_ != connected) {
      connected_ = connected; head_ = count_ = 0; held_ = 0;
    }
  }
  bool key(uint8_t code, bool released, uint32_t now) {
    const Action action = fromKey(code);
    if (!connected_ || action == Action::None) return false;
    if (released) { held_ &= uint16_t(~uint16_t(1u << (code - 0x44))); return false; }
    const uint16_t mask = uint16_t(1u << (code - 0x44));
    if (held_ & mask) return false;
    held_ |= mask;
    if (count_ == capacity) return false;
    events_[(head_ + count_) % capacity] = {action, now}; ++count_;
    return true;
  }
  Action pop(uint32_t now) {
    while (count_) {
      const Event event = events_[head_]; head_ = (head_ + 1) % capacity; --count_;
      if (connected_ && uint32_t(now - event.time) <= maxAgeMs) return event.action;
    }
    return Action::None;
  }
 private:
  struct Event { Action action; uint32_t time; };
  Event events_[capacity]{};
  size_t head_ = 0, count_ = 0;
  uint16_t held_ = 0;
  bool connected_ = false;
};
}
