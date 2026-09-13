// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Checked arithmetic.  Aggregation and counter accounting must never wrap
// silently; saturation is always reported.

#ifndef COHERENCE_CHECKED_HPP
#define COHERENCE_CHECKED_HPP

#include <cstdint>
#include <limits>

namespace sol::coherence {

/// Outcome of a checked accumulation.
struct AddResult {
  std::uint64_t value = 0;
  bool overflowed = false;
  /// True when the mathematical result exceeds the representable range and
  /// ef value holds the saturated result.
  bool saturated() const noexcept { return overflowed; }
};

/// Saturating add with explicit overflow reporting.
constexpr AddResult checked_add(std::uint64_t a, std::uint64_t b) noexcept {
  AddResult r;
  const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
  if (a > max - b) {
    r.value = max;
    r.overflowed = true;
  } else {
    r.value = a + b;
  }
  return r;
}

/// Saturating add against an explicit ceiling.
constexpr AddResult checked_add_capped(std::uint64_t a, std::uint64_t b,
                                       std::uint64_t cap) noexcept {
  AddResult r = checked_add(a, b);
  if (!r.overflowed && r.value > cap) {
    r.value = cap;
    r.overflowed = true;
  }
  return r;
}

/// True when the sum of p a and p b fits in c std::uint64_t.
constexpr bool add_fits(std::uint64_t a, std::uint64_t b) noexcept {
  return a <= std::numeric_limits<std::uint64_t>::max() - b;
}

/// Saturating multiply with explicit overflow reporting.
constexpr AddResult checked_mul(std::uint64_t a, std::uint64_t b) noexcept {
  AddResult r;
  if (a == 0 || b == 0) {
    return r;
  }
  const std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
  if (a > max / b) {
    r.value = max;
    r.overflowed = true;
  } else {
    r.value = a * b;
  }
  return r;
}

/// True when a floating point value is finite and within an absolute bound.
constexpr bool finite_within(double v, double bound) noexcept {
  return v == v && v <= bound && v >= -bound;
}

}  // namespace sol::coherence

#endif  // COHERENCE_CHECKED_HPP
