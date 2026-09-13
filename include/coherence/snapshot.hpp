// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Immutable read snapshots.
//
// A snapshot is a value.  Once captured it never changes, and it always states
// the coordinator and observation epochs it was taken under so that a caller
// can detect that it has been superseded.

#ifndef COHERENCE_SNAPSHOT_HPP
#define COHERENCE_SNAPSHOT_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "coherence/aggregate.hpp"
#include "coherence/attribution.hpp"
#include "coherence/cost.hpp"
#include "coherence/findings.hpp"
#include "coherence/ids.hpp"
#include "coherence/publisher.hpp"
#include "coherence/region.hpp"
#include "coherence/resource.hpp"
#include "coherence/topology.hpp"

namespace sol::coherence {

/// Historical evidence restored from durable state.  Never current.
struct COHERENCE_API HistoricalEvidence {
  MemoryRegionId region;
  EventType event_type = EventType::UnknownCoherenceEvent;
  std::uint64_t observations = 0;
  std::uint64_t bytes = 0;
  Nanos last_timestamp_ns = 0;
  Precision precision = Precision::Unknown;
  Provenance provenance = Provenance::Unknown;
  std::uint64_t source_coordinator_epoch = 0;
};

/// How and why the runtime lost evidence.
struct COHERENCE_API SnapshotLoss {
  LossReport loss;
  std::uint64_t aggregate_saturations = 0;
};

namespace detail {
struct SnapshotAssembler;
}  // namespace detail

/// Immutable point-in-time view of the observatory.
///
/// Snapshots are handed out only as c shared_ptr<const Snapshot>: the
/// containers are never exposed mutably and a snapshot is never updated after
/// it is captured.
class COHERENCE_API Snapshot {
 public:
  Snapshot() = default;

  // ---- Epochs and identity --------------------------------------------
  SnapshotGeneration generation() const noexcept { return generation_; }
  CoordinatorEpoch coordinator_epoch() const noexcept { return coordinator_epoch_; }
  ObservationEpoch observation_epoch() const noexcept { return observation_epoch_; }
  TopologyGeneration topology_generation() const noexcept { return topology_generation_; }
  Nanos captured_at_ns() const noexcept { return captured_at_ns_; }
  Reality reality() const noexcept { return reality_; }

  /// True when the snapshot still describes the live epochs.
  bool is_current(CoordinatorEpoch coordinator, ObservationEpoch observation) const noexcept;

  // ---- Structural state -------------------------------------------------
  const std::vector<PublisherView>& publishers() const noexcept { return publishers_; }
  const std::vector<NodeRecord>& nodes() const noexcept { return nodes_; }
  const std::vector<ProcessorRecord>& processors() const noexcept { return processors_; }
  const std::vector<AcceleratorRecord>& accelerators() const noexcept { return accelerators_; }
  const std::vector<MemoryDomainRecord>& memory_domains() const noexcept { return memory_domains_; }
  const std::vector<CoherenceDomainRecord>& coherence_domains() const noexcept {
    return coherence_domains_;
  }
  const std::vector<RegionRecord>& regions() const noexcept { return regions_; }
  const TopologyRecord& topology() const noexcept { return topology_; }

  // ---- Analysis ---------------------------------------------------------
  const AggregateStore& aggregates() const noexcept { return aggregates_; }
  /// Total aggregate buckets the coordinator held when the snapshot was taken.
  /// A snapshot received over the wire carries a bounded prefix of them.
  std::uint64_t aggregate_buckets_total() const noexcept { return aggregate_buckets_total_; }
  /// Buckets that did not fit the wire bound.  Zero for a local snapshot.
  std::uint64_t aggregate_buckets_omitted() const noexcept {
    return aggregate_buckets_omitted_;
  }
  const std::vector<Finding>& findings() const noexcept { return findings_; }
  const std::vector<StaleEvidenceRecord>& stale_evidence() const noexcept { return stale_; }
  const std::vector<Capability>& capabilities() const noexcept { return capabilities_; }
  const SnapshotLoss& loss() const noexcept { return loss_; }
  const CostModel& cost_model() const noexcept { return cost_model_; }

  // ---- History ----------------------------------------------------------
  /// Aggregate values restored from a previous coordinator incarnation.
  /// These are historical and are never counted as current evidence.
  const std::map<AggregateKey, AggregateValue>& historical_aggregates() const noexcept {
    return historical_aggregates_;
  }
  const std::vector<HistoricalEvidence>& history() const noexcept { return history_; }
  /// Number of historical aggregate buckets.  A snapshot received over the
  /// wire carries history as summary counts, not as full records.
  std::uint64_t historical_aggregate_count() const noexcept {
    return historical_aggregate_count_;
  }
  std::uint64_t historical_observation_count() const noexcept {
    return historical_observation_count_;
  }
  /// Coordinator epoch the historical data was produced under.
  CoordinatorEpoch history_source_epoch() const noexcept { return history_source_epoch_; }

  // ---- Convenience ------------------------------------------------------
  std::size_t current_publisher_count() const noexcept;
  std::size_t current_region_count() const noexcept;
  const PublisherView* find_publisher(const PublisherId& id) const noexcept;
  const RegionRecord* find_region(const MemoryRegionId& id) const noexcept;

 private:
  friend struct detail::SnapshotAssembler;

  SnapshotGeneration generation_;
  CoordinatorEpoch coordinator_epoch_;
  ObservationEpoch observation_epoch_;
  TopologyGeneration topology_generation_;
  Nanos captured_at_ns_ = 0;
  Reality reality_ = Reality::Real;

  std::vector<PublisherView> publishers_;
  std::vector<NodeRecord> nodes_;
  std::vector<ProcessorRecord> processors_;
  std::vector<AcceleratorRecord> accelerators_;
  std::vector<MemoryDomainRecord> memory_domains_;
  std::vector<CoherenceDomainRecord> coherence_domains_;
  std::vector<RegionRecord> regions_;
  TopologyRecord topology_;

  AggregateStore aggregates_;
  std::uint64_t aggregate_buckets_total_ = 0;
  std::uint64_t aggregate_buckets_omitted_ = 0;
  std::vector<Finding> findings_;
  std::vector<StaleEvidenceRecord> stale_;
  std::vector<Capability> capabilities_;
  SnapshotLoss loss_;
  CostModel cost_model_;

  std::map<AggregateKey, AggregateValue> historical_aggregates_;
  std::vector<HistoricalEvidence> history_;
  std::uint64_t historical_aggregate_count_ = 0;
  std::uint64_t historical_observation_count_ = 0;
  CoordinatorEpoch history_source_epoch_;
};

using SnapshotPtr = std::shared_ptr<const Snapshot>;

}  // namespace sol::coherence

#endif  // COHERENCE_SNAPSHOT_HPP
