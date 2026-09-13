// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Internal text helpers.  Not installed.

#ifndef COHERENCE_SRC_TEXT_UTIL_HPP
#define COHERENCE_SRC_TEXT_UTIL_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sol::coherence::detail {

/// ASCII case-insensitive equality.
bool iequals(std::string_view a, std::string_view b) noexcept;

/// Uppercase ASCII copy.
std::string to_upper(std::string_view text);

/// Joins strings with a separator.
std::string join(const std::vector<std::string>& parts, std::string_view separator);

/// Deterministic decimal formatting (no locale, no exponent for integers).
std::string format_u64(std::uint64_t value);
std::string format_i64(std::int64_t value);

/// Deterministic fixed-point formatting with p decimals digits after the point.
std::string format_double(double value, int decimals);

/// Renders a byte count with an explicit unit suffix.
std::string format_bytes(std::uint64_t bytes);

/// Truncates to at most p max_length bytes, appending "..." when cut.
std::string truncate(std::string_view text, std::size_t max_length);

/// FNV-1a 64-bit hash used for canonical content hashing.
std::uint64_t fnv1a_init() noexcept;
std::uint64_t fnv1a(std::uint64_t state, const void* data, std::size_t size) noexcept;
std::uint64_t fnv1a_u64(std::uint64_t state, std::uint64_t value) noexcept;
std::uint64_t fnv1a_str(std::uint64_t state, std::string_view text) noexcept;

}  // namespace sol::coherence::detail

#endif  // COHERENCE_SRC_TEXT_UTIL_HPP
