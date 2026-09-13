// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#ifndef COHERENCE_TIME_HPP
#define COHERENCE_TIME_HPP

#include <cstdint>
#include <string>

#include "coherence/error.hpp"
#include "coherence/export.hpp"

namespace sol::coherence {

/// Monotonic nanoseconds since an unspecified, process-local origin.
///
/// Coherence Observatory never compares monotonic timestamps across
/// processes; only ordering within one publisher boot is meaningful, and the
/// runtime treats cross-process timestamps as opaque ordering hints.
using Nanos = std::int64_t;

/// Monotonic clock reading, immune to wall-clock adjustments.
COHERENCE_API Nanos monotonic_now_ns() noexcept;

/// Wall-clock nanoseconds since the Unix epoch (reporting only, never ordering).
COHERENCE_API Nanos wall_clock_now_ns() noexcept;

/// Formats a duration in nanoseconds deterministically ("1234ns", "1.234us").
COHERENCE_API std::string format_duration_ns(Nanos ns);

}  // namespace sol::coherence

#endif  // COHERENCE_TIME_HPP
