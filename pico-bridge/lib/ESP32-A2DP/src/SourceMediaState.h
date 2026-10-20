#pragma once // Aurora Pico Bluetooth bridge addition, not part of upstream ESP32-A2DP (GPL-3.0-only); see ../MODIFICATIONS.md
#include <cstdint>

// Owned exclusively by the Bluetooth application task. AVDTP pause retains
// the connection: unlike the example's STOPPING state it never disconnects.
class SourceMediaState {
 public:
  enum class Command { None, Check, Start, Suspend };
  bool active() const { return pending_ == Command::None && started_; }
  void reset() { *this = {}; }
  void observe(bool started) { started_ = started; }
  void request_recovery() { recover_ = true; }
  Command next(uint32_t now, bool wanted) {
    if (!wanted) recover_ = false; // An explicit pause always wins recovery.
    if (pending_ != Command::None) {
      if (uint32_t(now - sent_at_) < 1000) return Command::None;
      pending_ = Command::None; // Lost ACK: retry without blocking UART/BT.
    }
    if (backoff_ && uint32_t(now - failed_at_) < 100) return Command::None;
    backoff_ = false;
    if (wanted == started_ && !recover_) return Command::None;
    pending_ = recover_ ? Command::Suspend :
        wanted ? (ready_ ? Command::Start : Command::Check) : Command::Suspend;
    sent_at_ = now;
    return pending_;
  }
  bool acknowledge(Command command, bool success, uint32_t now, bool busy=false) {
    if (command == Command::None || pending_ != command) return false;
    pending_ = Command::None;
    if (!success) {
      // A wedged/actually-stopped stream can reject SUSPEND. In recovery,
      // probe readiness and START instead of endlessly suspending it. BUSY
      // still waits, because another control operation may be in flight.
      if(command==Command::Suspend && recover_ && !busy) {
        started_=false;ready_=false;recover_=false;
      }
      if (command != Command::Suspend) ready_ = false;
      backoff_ = true; failed_at_ = now;
      return true;
    }
    if (command == Command::Check) ready_ = true;
    else if (command == Command::Start) started_ = true;
    else { started_ = false; ready_ = false; recover_ = false; }
    return true;
  }
 private:
  Command pending_ = Command::None;
  bool started_ = false, ready_ = false, backoff_ = false, recover_ = false;
  uint32_t sent_at_ = 0, failed_at_ = 0;
};
