// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// The runtime facade.
//
// The observatory owns ingestion, identity, evidence, attribution, analysis,
// snapshots and durable structural state.  It owns no coherence policy: it
// never arbitrates ownership, never issues invalidations, never migrates data
// and never decides consistency semantics for another runtime.

#ifndef COHERENCE_OBSERVATORY_HPP
#define COHERENCE_OBSERVATORY_HPP

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "coherence/aggregate.hpp"
#include "coherence/attribution.hpp"
#include "coherence/cost.hpp"
#include "coherence/findings.hpp"
#include "coherence/observation.hpp"
#include "coherence/persistence.hpp"
#include "coherence/publisher.hpp"
#include "coherence/region.hpp"
#include "coherence/resource.hpp"
#include "coherence/snapshot.hpp"
#include "coherence/topology.hpp"

namespace sol::coherence {

/// Publisher registration request.
struct COHERENCE_API PublisherRegistration {
  PublisherId id;
  PublisherBootId boot;
  ObserverId observer;
  NodeId node;
  std::string display_name;
  /// Coordinator epoch the publisher believes is live.  Zero means "any".
  /// A non-zero value that does not match is rejected.
  CoordinatorEpoch coordinator_epoch;
  EvidenceGeneration evidence_generation{1};
  SamplingEpoch sampling_epoch{1};
  Provenance provenance = Provenance::SyntheticBackend;
};

/// What happened to an offered observation.
enum class IngestionDisposition : std::uint8_t {
  /// Accepted, contributes to aggregation and analysis.
  Accepted = 0,
  /// Accepted but arrived out of order; flagged late.
  AcceptedLate = 1,
  /// Already seen: idempotent, not counted twice.
  Duplicate = 2,
  /// Rejected; nothing mutated.
  Rejected = 3,
};

COHERENCE_API std::string_view to_string(IngestionDisposition disposition) noexcept;

/// Result of offering one observation.
struct COHERENCE_API IngestionOutcome {
  IngestionDisposition disposition = IngestionDisposition::Rejected;
  ErrorCode code = ErrorCode::Ok;
  std::string detail;
  /// True when the observation contributed to aggregates.
  bool counted = false;
  /// Sequences discovered missing by accepting this observation.
  std::uint64_t missing_sequences = 0;
  /// Compact attribution summary decided at ingestion time.
  AttributionOutcome attribution = AttributionOutcome::Unattributed;
  std::string attribution_reason;
};

/// Aggregate result of offering a batch.
struct COHERENCE_API BatchOutcome {
  std::uint64_t accepted = 0;
  std::uint64_t accepted_late = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t rejected = 0;
  std::uint64_t missing_sequences = 0;
  std::vector<IngestionOutcome> per_event;

  std::size_t size() const noexcept { return per_event.size(); }
};

/// Result of offering a counter publication.
struct COHERENCE_API CounterOutcome {
  bool accepted = false;
  ErrorCode code = ErrorCode::Ok;
  std::string detail;
  /// True when this sample produced a usable positive delta.
  bool produced_delta = false;
  std::uint64_t delta = 0;
  /// True when the counter restarted (reset or generation change).
  bool discontinuity = false;
  std::string discontinuity_reason;
};

/// Options controlling how much analysis a snapshot performs.
struct COHERENCE_API FindingsOptions {
  bool compute_hotspots = true;
  bool compute_ping_pong = true;
  bool compute_false_sharing = true;
  bool compute_invalidation = true;
  bool compute_remote_access = true;
  bool include_operational = true;
  std::size_t max_findings = Limits::kMaxFindings;
};

/// Epoch set currently in force.
struct COHERENCE_API EpochSet {
  CoordinatorEpoch coordinator_epoch;
  ObservationEpoch observation_epoch;
  TopologyGeneration topology_generation;
  SnapshotGeneration snapshot_generation;
};

/// Outcome of reconciling current evidence against publication reality.
struct COHERENCE_API ReconcileReport {
  CoordinatorEpoch coordinator_epoch;
  ObservationEpoch observation_epoch;
  std::uint64_t publishers_marked_stale = 0;
  std::uint64_t evidence_generations_retired = 0;
  std::uint64_t stale_records = 0;
  std::uint64_t findings_invalidated = 0;
};

/// Behaviour when bounded ingestion capacity is reached.
enum class OverflowPolicy : std::uint8_t {
  /// Reject new observations and count the loss.
  Reject = 0,
  /// Accept, but count and report the loss.
  Shed = 1,
};

/// Configuration for an observatory instance.
struct COHERENCE_API ObservatoryOptions {
  ObserverId observer_id;
  NodeId node_id;
  std::string display_name;
  CoordinatorEpoch initial_coordinator_epoch{1};
  CostModel cost_model = default_cost_model();
  AttributionPolicy attribution_policy;
  HotspotPolicy hotspot_policy;
  PingPongPolicy ping_pong_policy;
  FalseSharingPolicy false_sharing_policy;
  InvalidationPolicy invalidation_policy;
  RemoteAccessPolicy remote_access_policy;
  /// Width of the aggregation time buckets.
  std::int64_t time_bucket_ns = 1000000000;
  /// Bound on queued-but-unprocessed observations.
  std::size_t max_pending_ingestion = Limits::kMaxPendingIngestion;
  OverflowPolicy overflow_policy = OverflowPolicy::Reject;
  /// When false, evidence from non-current publishers is still accepted and
  /// labelled stale rather than rejected.  Defaults to strict.
  bool enforce_current_publisher = true;
};

/// The Coherence Observatory runtime.
///
/// Thread-safety: all public methods are safe to call concurrently.  The
/// runtime uses one state mutex; no lock is ever held across socket I/O,
/// filesystem I/O, callbacks, vendor APIs or thread joins.  See README for the
/// complete lock order.
class COHERENCE_API Observatory {
 public:
  explicit Observatory(ObservatoryOptions options = {});
  ~Observatory();

  Observatory(const Observatory&) = delete;
  Observatory& operator=(const Observatory&) = delete;
  Observatory(Observatory&&) = delete;
  Observatory& operator=(Observatory&&) = delete;

  // ---- Epochs ----------------------------------------------------------
  EpochSet epochs() const;
  CoordinatorEpoch coordinator_epoch() const;
  ObservationEpoch observation_epoch() const;
  TopologyGeneration topology_generation() const;

  // ---- Structural registration ----------------------------------------
  Status register_node(NodeRecord record);
  Status register_processor(ProcessorRecord record);
  Status register_accelerator(AcceleratorRecord record);
  Status register_memory_domain(MemoryDomainRecord record);
  Status register_coherence_domain(CoherenceDomainRecord record);
  Status register_region(RegionRecord record);
  /// Retires a region at an exact generation.  A mismatched generation is
  /// rejected: a stale retirement request must not retire a live region.
  Status retire_region(const MemoryRegionId& region, MemoryRegionGeneration generation,
                       std::string reason);
  Status set_topology_link(TopologyLink link);
  /// Bumps the topology generation, invalidating locality bindings.
  Result<TopologyGeneration> bump_topology_generation(std::string reason);
  Status set_capability(Capability capability);
  Status set_cost_model(CostModel model);
  CostModel cost_model() const;

  // ---- Publisher authority --------------------------------------------
  Result<PublisherView> register_publisher(const PublisherRegistration& registration);
  /// Fences one publisher boot identity.  Its dynamic evidence stops being
  /// current immediately and can never be revived.
  Result<PublisherView> fence_publisher_boot(const PublisherId& publisher,
                                             PublisherBootId boot, FenceReason reason,
                                             std::string detail);
  /// Fences every boot of a publisher identity.
  Result<PublisherView> fence_publisher(const PublisherId& publisher, FenceReason reason,
                                        std::string detail);
  /// Fences every registered publisher boot (used on coordinator restart).
  Status fence_all_publishers(FenceReason reason, std::string detail);
  Result<PublisherView> publisher(const PublisherId& id) const;

  // ---- Ingestion -------------------------------------------------------
  Result<IngestionOutcome> ingest(Observation observation);
  Result<BatchOutcome> ingest_batch(std::vector<Observation> observations);
  Result<CounterOutcome> ingest_counter(CounterPublication publication);

  // ---- Attribution -----------------------------------------------------
  Result<AttributionResult> attribute(const Observation& observation) const;
  Result<AttributionResult> attribute_region(const MemoryRegionId& region) const;

  // ---- Queries ---------------------------------------------------------
  SnapshotPtr snapshot(const FindingsOptions& options = {}) const;
  Result<std::vector<Finding>> findings(const FindingsOptions& options = {}) const;
  Result<RegionAnalysis> analyze_region(const MemoryRegionId& region,
                                        const FindingsOptions& options = {}) const;
  Result<std::vector<RemoteAccessRow>> remote_access(const RemoteAccessPolicy& policy = {}) const;
  Result<std::vector<InvalidationRow>> invalidation_analysis(
      const InvalidationPolicy& policy = {}) const;
  Result<std::vector<Finding>> ping_pong(const PingPongPolicy& policy = {}) const;
  Result<std::vector<Finding>> hotspots(const HotspotPolicy& policy = {}) const;
  Result<std::vector<Finding>> false_sharing(const FalseSharingPolicy& policy = {}) const;

  // ---- Evidence reconciliation ----------------------------------------
  /// Re-evaluates which evidence is still current and marks the rest stale.
  Result<ReconcileReport> reconcile_current_evidence();

  // ---- Persistence -----------------------------------------------------
  Result<PersistenceReport> save_state(const std::filesystem::path& path,
                                       const PersistenceOptions& options = {});
  /// Loads durable state and performs conservative recovery.  On failure the
  /// live state is left completely untouched.
  Result<PersistenceReport> load_state(const std::filesystem::path& path,
                                       const PersistenceOptions& options = {});

  // ---- Diagnostics -----------------------------------------------------
  /// Deterministic 64-bit fingerprint of all live state.  Used to prove that
  /// rejected input mutated nothing.
  std::uint64_t state_fingerprint() const;
  LossReport loss_report() const;
  /// Deterministically sorted classification of every known capability.
  std::vector<Capability> capabilities() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sol::coherence

#endif  // COHERENCE_OBSERVATORY_HPP
