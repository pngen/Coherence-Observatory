// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Locality classification.
//
// Locality is never guessed from device presence.  It is either declared by
// the evidence source and validated against the topology generation it was
// produced under, or derived from a registered topology link at that same
// generation.

#ifndef COHERENCE_LOCALITY_HPP
#define COHERENCE_LOCALITY_HPP

#include <cstdint>
#include <string_view>

#include "coherence/error.hpp"
#include "coherence/export.hpp"

namespace sol::coherence {

enum class Locality : std::uint8_t {
  Unknown = 0,
  LocalProcessor = 1,
  LocalNuma = 2,
  RemoteNuma = 3,
  LocalAccelerator = 4,
  PeerAccelerator = 5,
  SameRootComplex = 6,
  RemoteAccelerator = 7,
  CxlAttached = 8,
  PooledMemory = 9,
  RemoteNode = 10,
};

inline constexpr std::uint8_t kLocalityCount = 11;

COHERENCE_API std::string_view to_string(Locality locality) noexcept;
COHERENCE_API Result<Locality> parse_locality(std::string_view text);

/// True when the class denotes memory reached across a domain boundary.
COHERENCE_API bool is_remote_locality(Locality locality) noexcept;

/// True when the class denotes CXL-class or pooled memory.
COHERENCE_API bool is_cxl_class_locality(Locality locality) noexcept;

}  // namespace sol::coherence

#endif  // COHERENCE_LOCALITY_HPP
