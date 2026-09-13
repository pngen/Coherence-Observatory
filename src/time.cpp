// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/time.hpp"

#include <chrono>

#include "text_util.hpp"

namespace sol::coherence {

Nanos monotonic_now_ns() noexcept {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

Nanos wall_clock_now_ns() noexcept {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

std::string format_duration_ns(Nanos ns) {
  if (ns < 0) {
    return std::string("unmeasured");
  }
  const double value = static_cast<double>(ns);
  if (value < 1000.0) {
    return detail::format_i64(ns) + "ns";
  }
  if (value < 1000000.0) {
    return detail::format_double(value / 1000.0, 3) + "us";
  }
  if (value < 1000000000.0) {
    return detail::format_double(value / 1000000.0, 3) + "ms";
  }
  return detail::format_double(value / 1000000000.0, 3) + "s";
}

}  // namespace sol::coherence
