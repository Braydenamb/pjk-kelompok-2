#pragma once
#include <stdint.h>
enum RetryAction { WAIT, RETRY, EXHAUSTED };
struct Pending {
  bool active = false;
  uint32_t session = 0, seq = 0, since = 0;
  uint8_t retries = 0;
  void begin(uint32_t s, uint32_t q, uint32_t now) {
    active = true; session = s; seq = q; since = now; retries = 0;
  }
  void clear() { active = false; retries = 0; }
  bool matches(uint32_t s, uint32_t q) const {
    return active && session == s && seq == q;
  }
  RetryAction poll(uint32_t now, uint32_t timeout, uint8_t limit) {
    if (!active || uint32_t(now - since) < timeout) return WAIT;
    if (retries >= limit) { clear(); return EXHAUSTED; }
    ++retries; since = now; return RETRY;
  }
};
