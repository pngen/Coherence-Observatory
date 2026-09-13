// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Generic coherence-event taxonomy and its associated enumerations.
//
// Backend-native events map onto this taxonomy conservatively.  When a backend
// event has no exact generic equivalent it must be published as
// EventType::UnknownCoherenceEvent with the backend-native name retained in the
// bounded observation metadata: the runtime never forces a false equivalence.

#ifndef COHERENCE_TAXONOMY_HPP
#define COHERENCE_TAXONOMY_HPP

#include <cstdint>
#include <string_view>

#include "coherence/error.hpp"
#include "coherence/export.hpp"

namespace sol::coherence {

/// Stable generic coherence event classes.
///
/// Every class listed here is implemented by the runtime: each has a
/// deterministic aggregation bucket, an attribution rule, and (where the
/// evidence supports it) an analyzer that consumes it.
enum class EventType : std::uint8_t {
  RemoteRead = 0,
  RemoteWrite = 1,
  Invalidation = 2,
  OwnershipTransfer = 3,
  OwnershipUpgrade = 4,
  OwnershipDowngrade = 5,
  SharedRead = 6,
  WriteExclusiveTransition = 7,
  CacheToCacheTransfer = 8,
  MemoryDomainTransfer = 9,
  SnoopRequest = 10,
  SnoopResponse = 11,
  DirectoryLookup = 12,
  DirectoryMiss = 13,
  CoherenceRetry = 14,
  CoherenceConflict = 15,
  Writeback = 16,
  RemoteAtomic = 17,
  ConsistencyBarrier = 18,
  Flush = 19,
  Fence = 20,
  RegionInvalidation = 21,
  UnknownCoherenceEvent = 22,

  kCount = 23,
};

/// Number of distinct event classes.
inline constexpr std::uint8_t kEventTypeCount =
    static_cast<std::uint8_t>(EventType::kCount);

COHERENCE_API std::string_view to_string(EventType type) noexcept;
COHERENCE_API Result<EventType> parse_event_type(std::string_view text);

/// True when the class carries ownership/state transition semantics and is
/// therefore eligible as ping-pong evidence.
COHERENCE_API bool is_ownership_event(EventType type) noexcept;

/// True when the class represents traffic that moves bytes between domains.
COHERENCE_API bool is_traffic_event(EventType type) noexcept;

/// Direction of the memory access described by an observation.
enum class AccessDirection : std::uint8_t {
  None = 0,
  Read = 1,
  Write = 2,
  ReadModifyWrite = 3,
  Atomic = 4,
  Unknown = 5,
};

COHERENCE_API std::string_view to_string(AccessDirection direction) noexcept;
COHERENCE_API Result<AccessDirection> parse_access_direction(std::string_view text);

/// Observable coherence state of a region or line.
enum class CoherenceState : std::uint8_t {
  Unknown = 0,
  Invalid = 1,
  Shared = 2,
  Exclusive = 3,
  Modified = 4,
  Owned = 5,
  Forward = 6,
  SharedClean = 7,
  SharedDirty = 8,
  ExclusiveDirty = 9,
};

COHERENCE_API std::string_view to_string(CoherenceState state) noexcept;
COHERENCE_API Result<CoherenceState> parse_coherence_state(std::string_view text);

/// Finest granularity at which the evidence actually resolves.
///
/// This is the guard against manufacturing precision: a socket-wide counter
/// must be published as Granularity::Device, never as Granularity::CacheLine.
enum class EvidenceGranularity : std::uint8_t {
  Unknown = 0,
  CacheLine = 1,
  Page = 2,
  Region = 3,
  Device = 4,
  Domain = 5,
  Aggregate = 6,
};

COHERENCE_API std::string_view to_string(EvidenceGranularity granularity) noexcept;
COHERENCE_API Result<EvidenceGranularity> parse_evidence_granularity(
    std::string_view text);

/// Rank of granularity: larger is finer.
COHERENCE_API std::uint8_t granularity_rank(EvidenceGranularity granularity) noexcept;

/// Kind of resource an identity refers to.
enum class ResourceKind : std::uint8_t {
  Unknown = 0,
  Node = 1,
  Processor = 2,
  Accelerator = 3,
  MemoryDomain = 4,
  CoherenceDomain = 5,
};

COHERENCE_API std::string_view to_string(ResourceKind kind) noexcept;
COHERENCE_API Result<ResourceKind> parse_resource_kind(std::string_view text);

/// Kind of counter, which determines reset/wrap handling.
enum class CounterKind : std::uint8_t {
  /// Monotonic absolute counter; a decrease is a reset (or wrap if declared).
  Absolute = 0,
  /// Value is already a delta for one sampling interval.
  Delta = 1,
  /// Absolute counter that the producer may reset at any time.
  Resettable = 2,
  /// Absolute counter with an explicit bit width; decrease of more than half
  /// the range is treated as a wrap.
  Wrapping = 3,
  /// Point-in-time sample; successive samples are not differences.
  Sampled = 4,
};

COHERENCE_API std::string_view to_string(CounterKind kind) noexcept;
COHERENCE_API Result<CounterKind> parse_counter_kind(std::string_view text);

/// What a counter's value can legitimately be attributed to.
enum class CounterScope : std::uint8_t {
  Unknown = 0,
  /// Genuinely per-region counter: region attribution is exact.
  PerRegion = 1,
  /// One counter per device/socket: region attribution is impossible.
  PerDevice = 2,
  /// One counter per memory domain.
  PerDomain = 3,
  /// One counter per interconnect link.
  PerLink = 4,
  /// One counter per process.
  PerProcess = 5,
  /// One counter for the whole system.
  Global = 6,
};

COHERENCE_API std::string_view to_string(CounterScope scope) noexcept;
COHERENCE_API Result<CounterScope> parse_counter_scope(std::string_view text);

/// Coherence cost accounting category.
enum class CostDimension : std::uint8_t {
  BytesTransferred = 0,
  LatencyNanos = 1,
  BandwidthBytesPerSecond = 2,
  StallCycles = 3,
  RetryOverhead = 4,
  InvalidatedBytes = 5,
  WritebackBytes = 6,
  RemoteMemoryAccesses = 7,
  OwnershipTransfers = 8,
  CrossDomainTraffic = 9,
  CrossNumaTraffic = 10,
  CrossAcceleratorTraffic = 11,
  CxlClassTraffic = 12,
  ExecutionTimeImpact = 13,

  kCount = 14,
};

COHERENCE_API std::string_view to_string(CostDimension dimension) noexcept;

}  // namespace sol::coherence

#endif  // COHERENCE_TAXONOMY_HPP
