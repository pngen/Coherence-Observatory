// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Memory-region identity.
//
// Regions are addressed by opaque identity.  Coherence Observatory never
// persists or exposes raw process virtual addresses: callers that only hold a
// pointer use make_opaque_handle(), which hashes the address with a
// process-local salt and discards it.

#ifndef COHERENCE_REGION_HPP
#define COHERENCE_REGION_HPP

#include <cstdint>
#include <string>

#include "coherence/ids.hpp"
#include "coherence/time.hpp"

namespace sol::coherence {

/// Who can observe this region.
enum class SharingScope : std::uint8_t {
  Unknown = 0,
  Private = 1,
  ProcessShared = 2,
  DeviceShared = 3,
  CrossNode = 4,
  Pooled = 5,
};

COHERENCE_API std::string_view to_string(SharingScope scope) noexcept;
COHERENCE_API Result<SharingScope> parse_sharing_scope(std::string_view text);

/// A memory region as registered by its owner runtime.
struct COHERENCE_API RegionRecord {
  MemoryRegionId id;
  MemoryRegionGeneration generation;
  /// Opaque, non-reversible handle derived from the owning runtime's handle.
  /// Never a raw virtual address.
  std::uint64_t opaque_handle = 0;
  /// Identity of the runtime or component that owns the region.
  std::string owner;
  MemoryDomainId memory_domain;
  MemoryDomainGeneration memory_domain_generation;
  CoherenceDomainId coherence_domain;
  SharingScope sharing_scope = SharingScope::Unknown;
  std::uint64_t size_bytes = 0;
  bool size_known = false;
  std::uint64_t page_size_bytes = 0;
  bool page_size_known = false;
  /// Generation of the allocation that produced this region.
  std::uint64_t allocation_generation = 0;
  /// Generation of the mapping through which it is visible.
  std::uint64_t mapping_generation = 0;
  WorkloadId workload;
  std::string annotation;
  bool retired = false;
  Nanos registered_at_ns = 0;
  Nanos retired_at_ns = 0;
  bool loaded_from_state = false;
};

/// Derives an opaque, non-reversible region handle from a process-local
/// address.  The address itself is never stored, serialized or reported.
COHERENCE_API std::uint64_t make_opaque_handle(const void* address,
                                               std::uint64_t owner_salt) noexcept;

/// Deterministic 64-bit fingerprint used for equality of opaque handles
/// without exposing the handle value.
COHERENCE_API std::uint64_t region_identity_fingerprint(const RegionRecord& record) noexcept;

}  // namespace sol::coherence

#endif  // COHERENCE_REGION_HPP
