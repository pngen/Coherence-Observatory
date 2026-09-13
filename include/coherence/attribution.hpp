// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Attribution.
//
// Attribution answers one question: given this evidence, what can safely be
// said about which current resource, generation, region, locality and cost it
// belongs to?  It never claims more precision than the evidence supports, and
// it distinguishes exact, partial, aggregate-only, ambiguous, unattributed,
// stale and unsupported outcomes.

#ifndef COHERENCE_ATTRIBUTION_HPP
#define COHERENCE_ATTRIBUTION_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "coherence/cost.hpp"
#include "coherence/ids.hpp"
#include "coherence/locality.hpp"
#include "coherence/observation.hpp"
#include "coherence/precision.hpp"
#include "coherence/provenance.hpp"

namespace sol::coherence {

/// What the runtime was able to conclude.
enum class AttributionOutcome : std::uint8_t {
  /// Bound to a specific region generation with evidence that resolves it.
  AttributedExact = 0,
  /// Bound to a resource/domain/locality but not to a region generation.
  AttributedPartial = 1,
  /// Only an aggregate bucket is defensible; no finer subject.
  AttributedAggregateOnly = 2,
  /// Several mutually exclusive subjects remain possible.
  Ambiguous = 3,
  /// No subject could be bound at all.
  Unattributed = 4,
  /// The evidence was superseded before it could be attributed.
  StaleEvidence = 5,
  /// The claim requires a capability this host/source does not provide.
  Unsupported = 6,
};

COHERENCE_API std::string_view to_string(AttributionOutcome outcome) noexcept;
COHERENCE_API Result<AttributionOutcome> parse_attribution_outcome(std::string_view text);

/// Kind of subject an attribution binds to.
enum class AttributionTargetKind : std::uint8_t {
  Resource = 0,
  MemoryDomain = 1,
  CoherenceDomain = 2,
  Region = 3,
  Workload = 4,
  Process = 5,
  TopologyEdge = 6,
  LocalityClass = 7,
  EventClass = 8,
};

COHERENCE_API std::string_view to_string(AttributionTargetKind kind) noexcept;

/// One subject an attribution binds to.
struct COHERENCE_API AttributionTarget {
  AttributionTargetKind kind = AttributionTargetKind::Resource;
  std::string value;
  std::uint64_t generation = 0;
};

/// A reference to the evidence supporting a result.
struct COHERENCE_API EvidenceRef {
  CoherenceEventId event_id;
  PublisherId publisher;
  PublisherBootId publisher_boot;
  EventSequence sequence;
  EventType event_type = EventType::UnknownCoherenceEvent;
  Precision precision = Precision::Unknown;
  Provenance provenance = Provenance::Unknown;
  Provenance provenance_class() const noexcept { return provenance; }
};

/// Generation bindings in force when the attribution was produced.
struct COHERENCE_API GenerationBindings {
  CoordinatorEpoch coordinator_epoch;
  ObservationEpoch observation_epoch;
  TopologyGeneration topology_generation;
  EvidenceGeneration evidence_generation;
  DeviceGeneration source_device_generation;
  DeviceGeneration target_device_generation;
  MemoryRegionGeneration region_generation;
  bool region_generation_bound = false;
  MemoryDomainGeneration source_domain_generation;
  MemoryDomainGeneration target_domain_generation;
};

/// Stable reason codes attached to attribution outcomes.
namespace attribution_reason {
inline constexpr std::string_view kBoundToRegionGeneration = "bound.region_generation";
inline constexpr std::string_view kBoundToDevice = "bound.device";
inline constexpr std::string_view kBoundToDomain = "bound.memory_domain";
inline constexpr std::string_view kBoundToLocality = "bound.locality_class";
inline constexpr std::string_view kBoundToEdge = "bound.topology_edge";
inline constexpr std::string_view kBoundToWorkload = "bound.workload";
inline constexpr std::string_view kBoundToProcess = "bound.process";
inline constexpr std::string_view kNoRegionIdentified = "missing.region_identity";
inline constexpr std::string_view kCoarseGranularity = "limited.evidence_granularity";
inline constexpr std::string_view kCounterScopeCoarser = "limited.counter_scope";
inline constexpr std::string_view kStaleCoordinatorEpoch = "stale.coordinator_epoch";
inline constexpr std::string_view kStalePublisher = "stale.publisher_not_current";
inline constexpr std::string_view kStaleRegionGeneration = "stale.region_generation";
inline constexpr std::string_view kRetiredRegion = "stale.region_retired";
inline constexpr std::string_view kStaleTopology = "stale.topology_generation";
inline constexpr std::string_view kUnknownResource = "unattributed.unknown_resource";
inline constexpr std::string_view kUnknownPublisher = "unattributed.unknown_publisher";
inline constexpr std::string_view kNoSubject = "unattributed.no_subject";
inline constexpr std::string_view kMultipleCandidates = "ambiguous.multiple_regions";
inline constexpr std::string_view kConflictingLocality = "ambiguous.locality_mismatch";
inline constexpr std::string_view kUnsupportedCapability = "unsupported.capability";
}  // namespace attribution_reason

/// Full attribution result.
struct COHERENCE_API AttributionResult {
  AttributionId id;
  AttributionOutcome outcome = AttributionOutcome::Unattributed;

  EventType event_type = EventType::UnknownCoherenceEvent;
  MemoryRegionId region;
  MemoryRegionGeneration region_generation;

  std::vector<std::string> reasons;
  std::vector<AttributionTarget> targets;
  std::vector<EvidenceRef> evidence;

  Precision precision = Precision::Unknown;
  Provenance provenance = Provenance::Unknown;
  Reality reality = Reality::Real;

  GenerationBindings bindings;

  /// Number of mutually exclusive candidate subjects that remained.
  std::uint64_t candidate_count = 0;
  std::vector<std::string> candidates;

  CostEstimate cost;

  /// Locality class established for this attribution.
  Locality locality = Locality::Unknown;
  bool locality_established = false;

  /// True when the result is bound tightly enough for exact per-region claims.
  bool exact() const noexcept { return outcome == AttributionOutcome::AttributedExact; }
  /// True when some subject was bound.
  bool bound() const noexcept {
    return outcome == AttributionOutcome::AttributedExact ||
           outcome == AttributionOutcome::AttributedPartial ||
           outcome == AttributionOutcome::AttributedAggregateOnly;
  }
  bool has_reason(std::string_view reason) const noexcept;
};

/// Policy controlling how strictly attribution enforces currentness.
struct COHERENCE_API AttributionPolicy {
  /// Reject evidence whose coordinator epoch is not the live one.
  bool require_current_coordinator_epoch = true;
  /// Reject evidence from a publisher boot that is not current.
  bool require_current_publisher = true;
  /// Treat evidence bound to a superseded topology generation as stale.
  bool require_current_topology = true;
  /// Treat evidence bound to a superseded region generation as stale.
  bool require_current_region_generation = true;
  /// Allow locality to be derived from a registered topology link.
  bool derive_locality_from_topology = true;
  /// Attributesat most this many candidate regions before declaring ambiguity.
  std::uint64_t max_candidates = Limits::kMaxAttributionCandidates;
};

}  // namespace sol::coherence

#endif  // COHERENCE_ATTRIBUTION_HPP
