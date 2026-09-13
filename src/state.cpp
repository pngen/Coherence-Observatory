// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// State application and snapshot assembly.

#include "detail/state.hpp"

#include <algorithm>
#include <string>

#include "detail/cost.hpp"
#include "text_util.hpp"

namespace sol::coherence {
namespace detail {
namespace {

bool is_region_relevant(EventType type) noexcept {
  return is_ownership_event(type) || type == EventType::Invalidation ||
         type == EventType::RegionInvalidation || type == EventType::RemoteWrite ||
         type == EventType::RemoteRead || type == EventType::CoherenceConflict ||
         type == EventType::CoherenceRetry || type == EventType::SharedRead ||
         type == EventType::Writeback || type == EventType::SnoopRequest ||
         type == EventType::SnoopResponse || type == EventType::DirectoryLookup ||
         type == EventType::DirectoryMiss || type == EventType::RemoteAtomic ||
         type == EventType::MemoryDomainTransfer;
}

/// Outcomes whose evidence is current and therefore counts.
bool counts_as_current_evidence(AttributionOutcome outcome) noexcept {
  switch (outcome) {
    case AttributionOutcome::StaleEvidence:
    case AttributionOutcome::Unsupported:
      return false;
    default:
      return true;
  }
}

void hash_bucket(std::uint64_t& h, const AggregateKey& key, const AggregateValue& value) {
  h = fnv1a_u64(h, static_cast<std::uint64_t>(key.dimension));
  h = fnv1a_str(h, key.value);
  h = fnv1a_u64(h, value.observations);
  h = fnv1a_u64(h, value.bytes);
  h = fnv1a_u64(h, value.remote_accesses);
  h = fnv1a_u64(h, value.remote_reads);
  h = fnv1a_u64(h, value.remote_writes);
  h = fnv1a_u64(h, value.invalidations);
  h = fnv1a_u64(h, value.ownership_transfers);
  h = fnv1a_u64(h, value.counter_delta_total);
  h = fnv1a_u64(h, value.saturated ? 1u : 0u);
  h = fnv1a_u64(h, static_cast<std::uint64_t>(value.weakest_precision));
}

}  // namespace

void record_stale(State& state, StaleEvidenceRecord record) {
  // A recorded detail is bounded here so that a long human-readable string can
  // never make a later serialization fail.
  record.detail = truncate(record.detail, 240);
  if (state.stale.size() >= Limits::kMaxFindings) {
    state.stale.erase(state.stale.begin());
  }
  state.stale.push_back(std::move(record));
}

void apply_observation(State& state, const Observation& observation,
                       AttributionOutcome outcome, Locality locality,
                       bool locality_established) {
  const bool counted = counts_as_current_evidence(outcome);
  if (counted) {
    const ObservationContribution contribution = contribution_of(observation);
    for (std::size_t index = 0; index < kAggregateDimensionCount; ++index) {
      const auto dimension = static_cast<AggregateDimension>(index);
      AggregateKey key;
      key.dimension = dimension;
      key.value = aggregate_value_text(observation, dimension, state.options.time_bucket_ns);
      if (!state.aggregates.accumulate(key, contribution)) {
        ++state.loss.dropped_aggregate_updates;
      }
    }
  }

  if (state.journal.size() >= Limits::kMaxObservationJournal) {
    state.journal.pop_front();
    ++state.loss.dropped_journal_entries;
  }
  JournalEntry entry;
  entry.observation = observation;
  entry.outcome = outcome;
  entry.locality = locality;
  entry.locality_established = locality_established;
  state.journal.push_back(std::move(entry));

  if (!counted || !observation.region.has_value() ||
      !is_region_relevant(observation.type)) {
    return;
  }

  const std::string key = observation.region->str();
  auto it = state.region_evidence.find(key);
  if (it == state.region_evidence.end()) {
    if (state.region_evidence.size() >= Limits::kMaxTrackedRegions) {
      // Deterministic FIFO eviction of the oldest tracked region.
      while (!state.region_evidence_order.empty()) {
        const std::string victim = state.region_evidence_order.front();
        state.region_evidence_order.pop_front();
        const auto erased = state.region_evidence.erase(victim);
        if (erased != 0) {
          ++state.loss.dropped_region_samples;
          break;
        }
      }
    }
    state.region_evidence_order.push_back(key);
    it = state.region_evidence.emplace(key, RegionEvidence{}).first;
  }

  RegionEvidence& evidence = it->second;
  ++evidence.relevant_total;
  if (evidence.samples.size() >= Limits::kMaxRegionSamples) {
    evidence.samples.pop_front();
    ++evidence.evicted;
    ++state.loss.dropped_region_samples;
  }
  OwnershipSample sample;
  sample.event_id = observation.event_id;
  sample.sequence = observation.sequence;
  sample.timestamp_ns = observation.timestamp_ns;
  if (observation.source.has_value()) {
    sample.from = *observation.source;
  }
  if (observation.target.has_value()) {
    sample.to = *observation.target;
  }
  sample.type = observation.type;
  sample.before = observation.state_before;
  sample.after = observation.state_after;
  sample.direction = observation.direction;
  sample.precision = observation.precision;
  sample.provenance = observation.provenance;
  sample.granularity = observation.granularity;
  sample.publisher = observation.source_publisher;
  sample.publisher_boot = observation.publisher_boot;
  if (observation.region_generation.has_value()) {
    sample.region_generation = *observation.region_generation;
  }
  if (const std::string* line = observation.metadata.find("line.index")) {
    sample.line_index = *line;
  }
  evidence.last_update_ns = observation.timestamp_ns;
  evidence.samples.push_back(std::move(sample));
}

std::uint64_t aggregate_fingerprint(const State& state) {
  std::uint64_t h = fnv1a_init();
  for (const auto& entry : state.aggregates.buckets()) {
    hash_bucket(h, entry.first, entry.second);
  }
  h = fnv1a_u64(h, state.aggregates.size());
  h = fnv1a_u64(h, state.aggregates.refused_updates());
  h = fnv1a_u64(h, state.aggregates.saturations());
  return h;
}

std::uint64_t state_fingerprint(const State& state) {
  std::uint64_t h = fnv1a_init();
  h = fnv1a_u64(h, state.coordinator_epoch.value());
  h = fnv1a_u64(h, state.observation_epoch.value());
  h = fnv1a_u64(h, state.topology_generation.value());
  h = fnv1a_u64(h, state.evidence_generation.value());

  for (const auto& entry : state.nodes) {
    h = fnv1a_str(h, entry.first);
    h = fnv1a_str(h, entry.second.display_name);
  }
  for (const auto& entry : state.processors) {
    h = fnv1a_str(h, entry.first);
    h = fnv1a_u64(h, entry.second.generation.value());
    h = fnv1a_u64(h, entry.second.logical_processor_count);
    h = fnv1a_u64(h, entry.second.retired ? 1u : 0u);
  }
  for (const auto& entry : state.accelerators) {
    h = fnv1a_str(h, entry.first);
    h = fnv1a_u64(h, entry.second.generation.value());
    h = fnv1a_str(h, entry.second.vendor_uuid);
    h = fnv1a_u64(h, entry.second.retired ? 1u : 0u);
  }
  for (const auto& entry : state.memory_domains) {
    h = fnv1a_str(h, entry.first);
    h = fnv1a_u64(h, entry.second.generation.value());
    h = fnv1a_u64(h, static_cast<std::uint64_t>(entry.second.kind));
    h = fnv1a_u64(h, entry.second.retired ? 1u : 0u);
  }
  for (const auto& entry : state.coherence_domains) {
    h = fnv1a_str(h, entry.first);
    h = fnv1a_u64(h, entry.second.generation.value());
    h = fnv1a_str(h, entry.second.protocol_family);
    h = fnv1a_u64(h, entry.second.retired ? 1u : 0u);
  }
  for (const auto& entry : state.regions) {
    h = region_identity_fingerprint(entry.second);
  }
  h = fnv1a_u64(h, state.topology.links.size());
  for (const TopologyLink& link : state.topology.links) {
    h = fnv1a_str(h, resource_text(link.from));
    h = fnv1a_str(h, resource_text(link.to));
    h = fnv1a_u64(h, static_cast<std::uint64_t>(link.locality));
  }
  for (const auto& entry : state.publishers) {
    h = fnv1a_str(h, entry.first);
    h = fnv1a_u64(h, entry.second.view.boot.value());
    h = fnv1a_u64(h, entry.second.view.current ? 1u : 0u);
    h = fnv1a_u64(h, entry.second.view.fenced ? 1u : 0u);
    h = fnv1a_u64(h, entry.second.view.accepted_events);
    h = fnv1a_u64(h, entry.second.high_watermark);
    h = fnv1a_u64(h, entry.second.window.size());
  }
  for (const auto& entry : state.counters) {
    h = fnv1a_str(h, entry.first.publisher);
    h = fnv1a_u64(h, entry.first.boot);
    h = fnv1a_str(h, entry.first.counter);
    h = fnv1a_u64(h, entry.second.generation.value());
    h = fnv1a_u64(h, entry.second.accumulated);
    h = fnv1a_u64(h, entry.second.resets);
    h = fnv1a_u64(h, entry.second.wraps);
  }
  for (const auto& entry : state.watermarks) {
    h = fnv1a_str(h, entry.first);
    h = fnv1a_u64(h, entry.second.high_watermark);
    h = fnv1a_u64(h, entry.second.fenced ? 1u : 0u);
  }
  h = fnv1a_u64(h, aggregate_fingerprint(state));
  h = fnv1a_u64(h, state.journal.size());
  for (const JournalEntry& entry : state.journal) {
    h = fnv1a_u64(h, entry.observation.content_hash());
    h = fnv1a_u64(h, static_cast<std::uint64_t>(entry.outcome));
  }
  h = fnv1a_u64(h, state.region_evidence.size());
  for (const auto& entry : state.region_evidence) {
    h = fnv1a_str(h, entry.first);
    h = fnv1a_u64(h, entry.second.relevant_total);
    h = fnv1a_u64(h, entry.second.evicted);
    for (const OwnershipSample& sample : entry.second.samples) {
      h = fnv1a_u64(h, sample.event_id.value());
      h = fnv1a_u64(h, sample.sequence.value());
      h = fnv1a_u64(h, static_cast<std::uint64_t>(sample.type));
      h = fnv1a_str(h, sample.from.id.view());
      h = fnv1a_str(h, sample.to.id.view());
    }
  }
  h = fnv1a_u64(h, state.stale.size());
  for (const StaleEvidenceRecord& record : state.stale) {
    h = fnv1a_u64(h, static_cast<std::uint64_t>(record.kind));
    h = fnv1a_u64(h, record.event_id.value());
    h = fnv1a_str(h, record.publisher.view());
    h = fnv1a_str(h, record.region.view());
    h = fnv1a_str(h, record.detail);
  }
  for (const auto& entry : state.capabilities) {
    h = fnv1a_str(h, entry.first);
    h = fnv1a_u64(h, static_cast<std::uint64_t>(entry.second.status));
    h = fnv1a_str(h, entry.second.detail);
  }
  h = fnv1a_u64(h, state.history.size());
  for (const HistoricalEvidence& record : state.history) {
    h = fnv1a_str(h, record.region.view());
    h = fnv1a_u64(h, record.observations);
    h = fnv1a_u64(h, record.bytes);
  }
  h = fnv1a_u64(h, state.historical_observation_count);
  h = fnv1a_u64(h, state.history_source_epoch.value());
  return h;
}

}  // namespace detail

namespace detail {

SnapshotPtr SnapshotAssembler::build(const State& state, const FindingsOptions& options) {
  {
    auto snapshot = std::make_shared<Snapshot>();
    snapshot->generation_ = state.snapshot_generation;
    snapshot->coordinator_epoch_ = state.coordinator_epoch;
    snapshot->observation_epoch_ = state.observation_epoch;
    snapshot->topology_generation_ = state.topology_generation;
    snapshot->captured_at_ns_ = monotonic_now_ns();
    snapshot->cost_model_ = state.options.cost_model;

    Reality reality = Reality::Real;
    bool any = false;
    for (const auto& entry : state.publishers) {
      snapshot->publishers_.push_back(entry.second.view);
      reality = any ? combine_reality(reality, entry.second.view.reality)
                    : entry.second.view.reality;
      any = true;
    }
    for (const auto& entry : state.nodes) {
      snapshot->nodes_.push_back(entry.second);
    }
    for (const auto& entry : state.processors) {
      snapshot->processors_.push_back(entry.second);
    }
    for (const auto& entry : state.accelerators) {
      snapshot->accelerators_.push_back(entry.second);
    }
    for (const auto& entry : state.memory_domains) {
      snapshot->memory_domains_.push_back(entry.second);
    }
    for (const auto& entry : state.coherence_domains) {
      snapshot->coherence_domains_.push_back(entry.second);
    }
    for (const auto& entry : state.regions) {
      snapshot->regions_.push_back(entry.second);
    }
    snapshot->topology_ = state.topology;
    snapshot->aggregates_ = state.aggregates;
    snapshot->aggregate_buckets_total_ = state.aggregates.buckets().size();
    snapshot->stale_ = state.stale;
    for (const auto& entry : state.capabilities) {
      snapshot->capabilities_.push_back(entry.second);
    }
    sort_capabilities(snapshot->capabilities_);
    snapshot->loss_.loss = state.loss;
    snapshot->loss_.aggregate_saturations = state.aggregates.saturations();
    snapshot->findings_ = analyze_all(state, options);
    snapshot->reality_ = any ? reality : Reality::Real;

    snapshot->historical_aggregates_ = state.historical_aggregates.buckets();
    snapshot->history_ = state.history;
    snapshot->historical_aggregate_count_ = state.historical_aggregates.buckets().size();
    snapshot->historical_observation_count_ = state.historical_observation_count;
    snapshot->history_source_epoch_ = state.history_source_epoch;
    return snapshot;
  }
}

void SnapshotAssembler::assign(Snapshot& out, std::uint64_t generation,
                               std::uint64_t coordinator, std::uint64_t observation,
                               std::uint64_t topology, std::int64_t captured, Reality reality,
                               std::vector<PublisherView> publishers,
                               std::vector<ProcessorRecord> processors,
                               std::vector<AcceleratorRecord> accelerators,
                               std::vector<MemoryDomainRecord> memory_domains,
                               std::vector<CoherenceDomainRecord> coherence_domains,
                               std::vector<RegionRecord> regions,
                               std::map<AggregateKey, AggregateValue> buckets,
                               std::uint64_t buckets_total, std::uint64_t buckets_omitted,
                               std::vector<Finding> findings,
                               std::vector<StaleEvidenceRecord> stale,
                               std::vector<Capability> capabilities, const LossReport& loss,
                               std::uint64_t saturations, std::uint64_t historical_observations,
                               std::uint64_t history_epoch, std::uint64_t historical_aggregates,
                               std::uint64_t history_records) {
  out.generation_ = SnapshotGeneration{generation};
  out.coordinator_epoch_ = CoordinatorEpoch{coordinator};
  out.observation_epoch_ = ObservationEpoch{observation};
  out.topology_generation_ = TopologyGeneration{topology};
  out.captured_at_ns_ = static_cast<Nanos>(captured);
  out.reality_ = reality;
  out.publishers_ = std::move(publishers);
  out.processors_ = std::move(processors);
  out.accelerators_ = std::move(accelerators);
  out.memory_domains_ = std::move(memory_domains);
  out.coherence_domains_ = std::move(coherence_domains);
  out.regions_ = std::move(regions);
  out.topology_.generation = out.topology_generation_;
  out.aggregates_.restore(std::move(buckets));
  out.aggregate_buckets_total_ = buckets_total;
  out.aggregate_buckets_omitted_ = buckets_omitted;
  out.findings_ = std::move(findings);
  out.stale_ = std::move(stale);
  out.capabilities_ = std::move(capabilities);
  out.loss_.loss = loss;
  out.loss_.aggregate_saturations = saturations;
  out.historical_aggregate_count_ = historical_aggregates;
  out.historical_observation_count_ = historical_observations;
  out.history_source_epoch_ = CoordinatorEpoch{history_epoch};
  out.history_.clear();
  if (history_records != 0) {
    // A remote snapshot reports history as summary counts only; the records
    // themselves remain on the coordinator.
    out.historical_aggregate_count_ = historical_aggregates;
  }
}

}  // namespace detail

std::size_t Snapshot::current_publisher_count() const noexcept {
  std::size_t count = 0;
  for (const PublisherView& publisher : publishers_) {
    if (publisher.current && !publisher.fenced) {
      ++count;
    }
  }
  return count;
}

std::size_t Snapshot::current_region_count() const noexcept {
  std::size_t count = 0;
  for (const RegionRecord& region : regions_) {
    if (!region.retired) {
      ++count;
    }
  }
  return count;
}

const PublisherView* Snapshot::find_publisher(const PublisherId& id) const noexcept {
  for (const PublisherView& publisher : publishers_) {
    if (publisher.id == id) {
      return &publisher;
    }
  }
  return nullptr;
}

const RegionRecord* Snapshot::find_region(const MemoryRegionId& id) const noexcept {
  for (const RegionRecord& region : regions_) {
    if (region.id == id) {
      return &region;
    }
  }
  return nullptr;
}

bool Snapshot::is_current(CoordinatorEpoch coordinator,
                          ObservationEpoch observation) const noexcept {
  return coordinator_epoch_ == coordinator && observation_epoch_ == observation;
}

}  // namespace sol::coherence
