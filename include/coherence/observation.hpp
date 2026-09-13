// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// The observation model.
//
// Observations are immutable evidence records.  Once accepted they are never
// mutated: aggregation, attribution and analysis all read them by const
// reference or by value copy.

#ifndef COHERENCE_OBSERVATION_HPP
#define COHERENCE_OBSERVATION_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "coherence/ids.hpp"
#include "coherence/limits.hpp"
#include "coherence/locality.hpp"
#include "coherence/precision.hpp"
#include "coherence/provenance.hpp"
#include "coherence/resource.hpp"
#include "coherence/taxonomy.hpp"
#include "coherence/time.hpp"

namespace sol::coherence {

/// Bounded, explicitly typed backend metadata.
///
/// Raw backend detail is retained so that unmappable events stay explainable,
/// but it never becomes authority: keys are unique, lengths are bounded, and
/// nothing in the analysis path reads metadata as a decision input.
class COHERENCE_API BoundedMetadata {
 public:
  /// Adds a key/value pair.  Fails with AlreadyExists on a duplicate key,
  /// TooLarge when a bound is exceeded, and TooMany past the entry limit.
  Status add(std::string_view key, std::string_view value);

  std::size_t size() const noexcept { return entries_.size(); }
  bool empty() const noexcept { return entries_.empty(); }

  const std::string& key_at(std::size_t index) const { return entries_[index].first; }
  const std::string& value_at(std::size_t index) const { return entries_[index].second; }

  const std::string* find(std::string_view key) const noexcept;
  bool contains(std::string_view key) const noexcept { return find(key) != nullptr; }

  /// Sorted (key, value) view for canonical serialization and rendering.
  std::vector<std::pair<std::string, std::string>> sorted_entries() const;

  /// 64-bit FNV-1a over the canonical (sorted) encoding.
  std::uint64_t canonical_hash() const noexcept;

 private:
  std::vector<std::pair<std::string, std::string>> entries_;
};

/// One immutable coherence observation.
struct COHERENCE_API Observation {
  // ---- Event identity ------------------------------------------------
  CoherenceEventId event_id;
  EventType type = EventType::UnknownCoherenceEvent;

  // ---- Publisher authority -------------------------------------------
  PublisherId source_publisher;
  PublisherBootId publisher_boot;
  CoordinatorEpoch coordinator_epoch;
  SamplingEpoch sampling_epoch;
  EvidenceGeneration evidence_generation;
  EventSequence sequence;

  // ---- Time -----------------------------------------------------------
  Nanos timestamp_ns = 0;

  // ---- Subject / object ----------------------------------------------
  std::optional<ResourceRef> source;
  std::optional<ResourceRef> target;
  std::optional<MemoryRegionId> region;
  std::optional<MemoryRegionGeneration> region_generation;
  std::optional<CoherenceDomainId> coherence_domain;
  std::optional<CoherenceDomainGeneration> coherence_domain_generation;
  TopologyGeneration topology_generation;
  std::optional<WorkloadId> workload;
  std::optional<ProcessId> process;

  // ---- Payload ---------------------------------------------------------
  AccessDirection direction = AccessDirection::None;
  CoherenceState state_before = CoherenceState::Unknown;
  CoherenceState state_after = CoherenceState::Unknown;
  /// Present only for counter-derived observations.
  std::optional<std::uint64_t> counter_delta;
  CounterGeneration counter_generation;
  std::uint64_t bytes = 0;
  std::uint64_t lines = 0;
  std::uint64_t pages = 0;
  std::uint64_t region_count = 0;
  /// Measured duration supplied by a real source; negative means unmeasured.
  std::int64_t measured_duration_ns = -1;

  // ---- Classification --------------------------------------------------
  Locality locality = Locality::Unknown;
  bool locality_declared = false;
  Provenance provenance = Provenance::Unknown;
  Precision precision = Precision::Unknown;
  EvidenceGranularity granularity = EvidenceGranularity::Unknown;
  BoundedMetadata metadata;

  /// The epoch at which the runtime accepted this observation.
  ObservationEpoch accepted_epoch;
  /// True when the observation arrived after one with a higher sequence.
  bool late = false;

  /// Canonical content hash; used for duplicate-versus-conflict detection.
  std::uint64_t content_hash() const noexcept;

  /// Reality implied by the provenance.
  Reality reality() const noexcept { return reality_of(provenance); }
};

/// Validation of a single observation against structural rules that do not
/// depend on runtime state (identity tokens, bounds, enum ranges,
/// source/target self-consistency).
COHERENCE_API Status validate_observation_structure(const Observation& observation);

/// A counter publication: the only path by which raw counters enter.
struct COHERENCE_API CounterPublication {
  PublisherId publisher;
  PublisherBootId publisher_boot;
  CoordinatorEpoch coordinator_epoch;
  EvidenceGeneration evidence_generation;
  CounterId counter;
  /// Coherence event class this counter measures.  A counter whose meaning is
  /// not a coherence class must not be published here.
  EventType mapped_type = EventType::UnknownCoherenceEvent;
  CounterKind kind = CounterKind::Absolute;
  CounterScope scope = CounterScope::Unknown;
  std::uint32_t width_bits = 64;
  CounterGeneration generation;
  SamplingEpoch sampling_epoch;
  /// Absolute reading, or a delta when kind == CounterKind::Delta.
  std::uint64_t raw_value = 0;
  /// Unit scale applied to the counter value to obtain bytes.  Zero means the
  /// counter does not measure traffic and must not be used for byte cost.
  std::uint64_t bytes_per_unit = 0;
  EventSequence sequence;
  Nanos timestamp_ns = 0;
  std::optional<ResourceRef> source;
  std::optional<ResourceRef> target;
  std::optional<MemoryRegionId> region;
  std::optional<MemoryRegionGeneration> region_generation;
  std::optional<WorkloadId> workload;
  TopologyGeneration topology_generation;
  Provenance provenance = Provenance::Unknown;
  EvidenceGranularity granularity = EvidenceGranularity::Unknown;
  BoundedMetadata metadata;
};

/// Structural validation of a counter publication.
COHERENCE_API Status validate_counter_publication(const CounterPublication& publication);

}  // namespace sol::coherence

#endif  // COHERENCE_OBSERVATION_HPP
