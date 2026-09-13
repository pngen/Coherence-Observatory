// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#ifndef COHERENCE_VERSION_HPP
#define COHERENCE_VERSION_HPP

#include <cstdint>
#include <string_view>

#include "coherence/export.hpp"

#define COHERENCE_OBSERVATORY_VERSION_MAJOR 1
#define COHERENCE_OBSERVATORY_VERSION_MINOR 0
#define COHERENCE_OBSERVATORY_VERSION_PATCH 0

namespace sol::coherence {

/// Semantic version of the Coherence Observatory library.
struct Version {
  std::uint32_t major = COHERENCE_OBSERVATORY_VERSION_MAJOR;
  std::uint32_t minor = COHERENCE_OBSERVATORY_VERSION_MINOR;
  std::uint32_t patch = COHERENCE_OBSERVATORY_VERSION_PATCH;

  friend constexpr bool operator==(const Version&, const Version&) = default;
  friend constexpr auto operator<=>(const Version&, const Version&) = default;
};

/// Version of the compiled library.
COHERENCE_API Version library_version() noexcept;

/// Version string, e.g. "1.0.0".
COHERENCE_API std::string_view library_version_string() noexcept;

/// Numeric wire-protocol version implemented by this library.
COHERENCE_API std::uint16_t protocol_version() noexcept;

/// Monotonic build identity string: version plus compiler/configuration tag.
COHERENCE_API std::string_view build_identity() noexcept;

}  // namespace sol::coherence

#endif  // COHERENCE_VERSION_HPP
