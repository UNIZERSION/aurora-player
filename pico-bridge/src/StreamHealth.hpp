#pragma once
#include <cstdint>

// Main-loop owner. Measures transport progress, never signal amplitude: a
// silent song, zero volume and long quiet passages are healthy PCM traffic.
class StreamHealth {
 public:
  enum class Action { None, RecoverTransport, ReportInputStall, TransportFailed };
  Action update(uint32_t now, bool wanted, bool connected, bool diagnostic,
                bool started, uint32_t received, uint32_t callbacks,
                uint32_t consumed) {
    if (!wanted || !connected || diagnostic) {
      tracking_ = false;
      return Action::None;
    }
    if (failed_) return Action::None;
    if (!tracking_) {
      tracking_ = true;
      since_ = rx_at_ = callback_at_ = consumed_at_ = now;
      rx_ = received; callbacks_ = callbacks; consumed_ = consumed;
      return Action::None;
    }
    if (received != rx_) { rx_ = received; rx_at_ = now; }
    if (callbacks != callbacks_) { callbacks_ = callbacks; callback_at_ = now; }
    if (consumed != consumed_) { consumed_ = consumed; consumed_at_ = now; }
    if (uint32_t(now - since_) < 3000) return Action::None;
    if (uint32_t(now - rx_at_) >= 1500) {
      if (!input_reported_ || uint32_t(now - input_report_at_) >= 10000) {
        input_reported_ = true; input_report_at_ = now;
        return Action::ReportInputStall;
      }
      return Action::None;
    }
    const bool stalled = !started || uint32_t(now - callback_at_) >= 1500 ||
                         uint32_t(now - consumed_at_) >= 1500;
    if (!stalled || (recovered_ && uint32_t(now - recovery_at_) < 15000))
      return Action::None;
    if (!attempts_ || uint32_t(now-window_at_)>=60000) {
      attempts_=0;window_at_=now;
    }
    if (attempts_>=3) { failed_=true;return Action::TransportFailed; }
    ++attempts_;
    recovered_ = true; recovery_at_ = since_ = now;
    return Action::RecoverTransport;
  }
  void session() { *this={}; }
 private:
  bool tracking_ = false, recovered_ = false, input_reported_ = false;
  bool failed_=false;unsigned attempts_=0;uint32_t window_at_=0;
  uint32_t since_ = 0, rx_at_ = 0, callback_at_ = 0, consumed_at_ = 0;
  uint32_t rx_ = 0, callbacks_ = 0, consumed_ = 0;
  uint32_t recovery_at_ = 0, input_report_at_ = 0;
};
