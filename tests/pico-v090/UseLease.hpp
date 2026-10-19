#pragma once
#include <cstdint>

// Queries never renew the lease. Only an explicit USE from the player does.
class UseLease {
 public:
  static constexpr uint32_t timeoutMs = 15000;
  void renew(uint32_t now) { lastUse_ = now; }
  bool expired(uint32_t now) const { return uint32_t(now - lastUse_) >= timeoutMs; }
 private:
  uint32_t lastUse_ = 0;
};
