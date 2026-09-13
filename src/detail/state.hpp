// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Internal runtime state.  Not installed and not part of the public API.
//
// Locking discipline: every field in State is protected by the single mutex
// owned by Observatory::Impl.  No function in this header takes any lock, and
// none of them performs socket, filesystem, vendor or callback work.

#ifndef COHERENCE_SRC_DETAIL_STATE_HPP
#define COHERENCE_SRC_DETAIL_STATE_HPP

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "coherence/aggregate.hpp"
#include "coherence/attribution.hpp"
#include "coherence/cost.hpp"
#include "coherence/findings.hpp"
#include "coherence/observatory.hpp"
#include "coherence/provenance.hpp"
#include "coherence/region.hpp"
#include "coherence/resource.hpp"
#include "coherence/snapshot.hpp"
#include "coherence/topology.hpp"
#include "coherence/version.hpp"

namespace sol::coherence::detail {

/// Sequence-window bookkeeping for one publisher boot.
struct PublisherState {
  PublisherView view;

  /// True once at least one sequence has been accepted for this boot.
  bool has_watermark = false;
  /// Highest sequence accepted.
  std::uint64_t high_watermark = 0;
  /// True when the watermark was restored from durable state.  Every sequence
  /// at or below it was already accepted by a previous coordinator
  /// incarnation, so re-sending one is a replay and is never merely "late".
  bool durable_watermark = false;
  /// Sequences inside the retention window mapped to their content hash.
  std::map<std::uint64_t, std::uint64_t> window;
  /// FIFO order of window entries for bounded eviction.
  std::deque<std::uint64_t> window_order;
  /// Recently seen event ids mapped to their content hash.
  std::map<std::uint64_t, std::uint64_t> recent_event_ids;
  std::deque<std::uint64_t> recent_event_order;

  /// Boot incarnations observed for this publisher identity across restarts.
  std::uint64_t boot_incarnations = 1;
};

/// Durable replay-protection watermark for a (publisher, boot) pair.
struct PublisherWatermark {
  PublisherId publisher;
  PublisherBootId boot;
  std::uint64_t high_watermark = 0;
  bool has_watermark = false;
  FenceReason fence_reason = FenceReason::NotFenced;
  bool fenced = false;
  std::uint64_t boot_incarnations = 1;
  Nanos last_seen_ns = 0;
};

/// Identity of one counter.
struct CounterKey {
  std::string publisher;
  std::uint64_t boot = 0;
  std::string counter;

  friend bool operator==(const CounterKey&, const CounterKey&) = default;
  friend auto operator<=>(const CounterKey&, const CounterKey&) = default;
};

/// Continuity state for one counter.
struct CounterState {
  CounterKind kind = CounterKind::Absolute;
  CounterScope scope = CounterScope::Unknown;
  std::uint32_t width_bits = 64;
  CounterGeneration generation;
  std::uint64_t bytes_per_unit = 0;
  EventType mapped_type = EventType::UnknownCoherenceEvent;

  bool has_last = false;
  std::uint64_t last_raw = 0;
  std::uint64_t accumulated = 0;
  bool saturated = false;
  std::uint64_t samples = 0;
  std::uint64_t resets = 0;
  std::uint64_t wraps = 0;
  bool has_time = false;
  Nanos first_ns = 0;
  Nanos last_ns = 0;

  Provenance provenance = Provenance::Unknown;
  EvidenceGranularity granularity = EvidenceGranularity::Unknown;
  std::optional<ResourceRef> source;
  std::optional<ResourceRef> target;
  std::optional<MemoryRegionId> region;
  std::optional<MemoryRegionGeneration> region_generation;
  std::optional<WorkloadId> workload;
  TopologyGeneration topology_generation;
};

/// One accepted observation plus its ingestion-time attribution.
struct JournalEntry {
  Observation observation;
  AttributionOutcome outcome = AttributionOutcome::Unattributed;
  /// Locality the runtime established for this evidence (declared by the
  /// source or derived from the bound topology generation).  Kept separate
  /// from the observation so that derived and declared locality stay
  /// distinguishable.
  Locality locality = Locality::Unknown;
  bool locality_established = false;
};

/// One ownership/contention-relevant sample retained for pattern analysis.
struct OwnershipSample {
  CoherenceEventId event_id;
  EventSequence sequence;
  Nanos timestamp_ns = 0;
  ResourceRef from;
  ResourceRef to;
  EventType type = EventType::UnknownCoherenceEvent;
  CoherenceState before = CoherenceState::Unknown;
  CoherenceState after = CoherenceState::Unknown;
  AccessDirection direction = AccessDirection::None;
  Precision precision = Precision::Unknown;
  Provenance provenance = Provenance::Unknown;
  EvidenceGranularity granularity = EvidenceGranularity::Unknown;
  PublisherId publisher;
  PublisherBootId publisher_boot;
  MemoryRegionGeneration region_generation;
  /// Backend-supplied line identity when the evidence genuinely resolves one.
  std::string line_index;
};

/// Bounded per-region evidence used by pattern analyzers.
struct RegionEvidence {
  std::deque<OwnershipSample> samples;
  std::uint64_t relevant_total = 0;
  std::uint64_t evicted = 0;
  Nanos last_update_ns = 0;
};

/// The complete authoritative runtime state.
struct State {
  ObservatoryOptions options;

  CoordinatorEpoch coordinator_epoch{1};
  ObservationEpoch observation_epoch{1};
  TopologyGeneration topology_generation{1};
  SnapshotGeneration snapshot_generation{1};
  EvidenceGeneration evidence_generation{1};

  std::map<std::string, NodeRecord> nodes;
  std::map<std::string, ProcessorRecord> processors;
  std::map<std::string, AcceleratorRecord> accelerators;
  std::map<std::string, MemoryDomainRecord> memory_domains;
  std::map<std::string, CoherenceDomainRecord> coherence_domains;
  std::map<std::string, RegionRecord> regions;
  TopologyRecord topology;

  std::map<std::string, PublisherState> publishers;
  std::map<CounterKey, CounterState> counters;
  /// Replay watermarks for boots that are no longer current.
  std::map<std::string, PublisherWatermark> watermarks;

  AggregateStore aggregates;
  AggregateStore historical_aggregates;
  std::deque<JournalEntry> journal;
  std::map<std::string, RegionEvidence> region_evidence;
  std::deque<std::string> region_evidence_order;
  std::vector<StaleEvidenceRecord> stale;
  std::map<std::string, Capability> capabilities;
  std::vector<HistoricalEvidence> history;
  std::uint64_t historical_observation_count = 0;
  CoordinatorEpoch history_source_epoch;

  LossReport loss;
  std::uint64_t attestations_accepted = 0;
  std::uint64_t attestations_rejected = 0;

  AttributionId next_attribution{1};
  FindingId next_finding{1};
  Nanos started_at_ns = 0;
  bool schema_configured = false;
  /// Admitted-but-unprocessed ingestion units.  Zero at rest.
  std::size_t pending_ingestion = 0;
};

// ---- Structural helpers ------------------------------------------------

/// Canonical key for a (publisher, boot) pair.
std::string boot_key(const PublisherId& publisher, PublisherBootId boot);

/// Stable canonical text for a resource reference.
std::string resource_text(const ResourceRef& ref);

/// True when the resource reference resolves to a registered resource of the
/// stated kind and generation.
bool resource_is_current(const State& state, const ResourceRef& ref);

/// Bumps the topology generation, invalidating locality bindings.
TopologyGeneration bump_topology(State& state, std::string reason);

// ---- Evidence application ---------------------------------------------

/// Applies an already-validated observation to aggregates, journal and
/// region evidence.  Records \p outcome as the ingestion-time attribution.
void apply_observation(State& state, const Observation& observation,
                       AttributionOutcome outcome, Locality locality = Locality::Unknown,
                       bool locality_established = false);

/// Records a stale-evidence entry, bounded by Limits::kMaxFindings.
void record_stale(State& state, StaleEvidenceRecord record);

// ---- Attribution -------------------------------------------------------

AttributionResult attribute_observation(const State& state, const Observation& observation);
AttributionResult attribute_region_state(const State& state, const MemoryRegionId& region);

// ---- Cost --------------------------------------------------------------

/// Estimates the coherence cost of one observation.
Precision effective_precision(const State& state, const Observation& observation);

// ---- Analysis ----------------------------------------------------------

std::vector<Finding> analyze_hotspots(const State& state, const HotspotPolicy& policy);
std::vector<Finding> analyze_ping_pong(const State& state, const PingPongPolicy& policy);
std::vector<Finding> analyze_false_sharing(const State& state, const FalseSharingPolicy& policy);
std::vector<Finding> analyze_invalidations(const State& state, const InvalidationPolicy& policy,
                                           std::vector<InvalidationRow>* rows);
std::vector<Finding> analyze_remote_access(const State& state, const RemoteAccessPolicy& policy,
                                           std::vector<RemoteAccessRow>* rows);
std::vector<Finding> analyze_operational(const State& state);
std::vector<Finding> analyze_all(const State& state, const FindingsOptions& options);

/// Sorts findings by their deterministic ordering key and truncates to
/// \p max_findings, recording the number dropped.
std::size_t finalize_findings(std::vector<Finding>& findings, std::size_t max_findings);

// ---- Snapshots ---------------------------------------------------------

/// Assembles snapshots for both the local runtime and the wire codec.
struct SnapshotAssembler {
  static SnapshotPtr build(const State& state, const FindingsOptions& options);

  /// Populates p out from decoded values.  Used by the wire codec.
  static void assign(Snapshot& out, std::uint64_t generation, std::uint64_t coordinator,
                     std::uint64_t observation, std::uint64_t topology, std::int64_t captured,
                     Reality reality, std::vector<PublisherView> publishers,
                     std::vector<ProcessorRecord> processors,
                     std::vector<AcceleratorRecord> accelerators,
                     std::vector<MemoryDomainRecord> memory_domains,
                     std::vector<CoherenceDomainRecord> coherence_domains,
                     std::vector<RegionRecord> regions,
                     std::map<AggregateKey, AggregateValue> buckets,
                     std::uint64_t buckets_total, std::uint64_t buckets_omitted,
                     std::vector<Finding> findings, std::vector<StaleEvidenceRecord> stale,
                     std::vector<Capability> capabilities, const LossReport& loss,
                     std::uint64_t saturations, std::uint64_t historical_observations,
                     std::uint64_t history_epoch, std::uint64_t historical_aggregates,
                     std::uint64_t history_records);
};

SnapshotPtr build_snapshot(const State& state, const FindingsOptions& options);

// ---- Diagnostics -------------------------------------------------------

/// Fingerprint of authoritative state.
///
/// Covers registrations, epochs, publishers, counters, aggregates, journal,
/// region evidence, stale records, capabilities and history.  Excludes the
/// loss report and monotonic id allocators, which are diagnostic rather than
/// authoritative: this lets a test prove that rejected input changed nothing
/// that matters while still observing the loss counters move.
std::uint64_t state_fingerprint(const State& state);

/// Fingerprint of current aggregation only.
std::uint64_t aggregate_fingerprint(const State& state);

}  // namespace sol::coherence::detail

#endif  // COHERENCE_SRC_DETAIL_STATE_HPP
