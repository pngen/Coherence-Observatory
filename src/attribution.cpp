// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/attribution.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include "detail/cost.hpp"
#include "detail/state.hpp"
#include "text_util.hpp"

namespace sol::coherence::detail {
namespace {

/// Capability keys that gate real-vs-synthetic observability claims.
constexpr std::string_view kCapabilityCacheLineOwnership = "coherence.cache_line_ownership";
constexpr std::string_view kCapabilityCxlTelemetry = "memory.cxl_telemetry";

CapabilityStatus capability_status(const State& state, std::string_view key) {
  const auto it = state.capabilities.find(std::string(key));
  if (it == state.capabilities.end()) {
    return CapabilityStatus::Unsupported;
  }
  return it->second.status;
}

void add_target(AttributionResult& result, AttributionTargetKind kind, std::string value,
                std::uint64_t generation) {
  if (value.empty()) {
    return;
  }
  if (result.targets.size() >= Limits::kMaxAttributionTargets) {
    return;
  }
  for (const AttributionTarget& existing : result.targets) {
    if (existing.kind == kind && existing.value == value &&
        existing.generation == generation) {
      return;
    }
  }
  AttributionTarget target;
  target.kind = kind;
  target.value = std::move(value);
  target.generation = generation;
  result.targets.push_back(std::move(target));
}

void add_reason(AttributionResult& result, std::string_view reason) {
  const std::string text(reason);
  if (std::find(result.reasons.begin(), result.reasons.end(), text) == result.reasons.end()) {
    result.reasons.push_back(text);
  }
}

void add_evidence(AttributionResult& result, const Observation& observation) {
  if (result.evidence.size() >= Limits::kMaxEvidenceRefs) {
    return;
  }
  EvidenceRef ref;
  ref.event_id = observation.event_id;
  ref.publisher = observation.source_publisher;
  ref.publisher_boot = observation.publisher_boot;
  ref.sequence = observation.sequence;
  ref.event_type = observation.type;
  ref.precision = observation.precision;
  ref.provenance = observation.provenance;
  result.evidence.push_back(ref);
}

void add_resource_target(AttributionResult& result, const ResourceRef& ref) {
  if (ref.empty()) {
    return;
  }
  switch (ref.kind) {
    case ResourceKind::MemoryDomain:
      add_target(result, AttributionTargetKind::MemoryDomain, ref.id.str(), ref.generation);
      break;
    case ResourceKind::CoherenceDomain:
      add_target(result, AttributionTargetKind::CoherenceDomain, ref.id.str(), ref.generation);
      break;
    case ResourceKind::Node:
    case ResourceKind::Processor:
    case ResourceKind::Accelerator:
    case ResourceKind::Unknown:
      add_target(result, AttributionTargetKind::Resource, ref.id.str(), ref.generation);
      break;
  }
}

bool binds_any_subject(const AttributionResult& result) {
  for (const AttributionTarget& target : result.targets) {
    if (target.kind != AttributionTargetKind::EventClass) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> regions_for_workload(const State& state, const WorkloadId& workload) {
  std::vector<std::string> found;
  for (const auto& entry : state.regions) {
    if (entry.second.retired) {
      continue;
    }
    if (entry.second.workload == workload) {
      found.push_back(entry.first);
      if (found.size() > Limits::kMaxAttributionCandidates) {
        break;
      }
    }
  }
  return found;
}

}  // namespace

std::string resource_text(const ResourceRef& ref) {
  if (ref.empty()) {
    return std::string("unknown");
  }
  std::string out(to_string(ref.kind));
  out.append(":");
  out.append(ref.id.view());
  out.append("@");
  out.append(detail::format_u64(ref.generation));
  return out;
}

std::string boot_key(const PublisherId& publisher, PublisherBootId boot) {
  std::string key = publisher.str();
  key.push_back('#');
  key.append(detail::format_u64(boot.value()));
  return key;
}

bool resource_is_current(const State& state, const ResourceRef& ref) {
  switch (ref.kind) {
    case ResourceKind::Processor: {
      const auto it = state.processors.find(ref.id.str());
      return it != state.processors.end() && !it->second.retired &&
             it->second.generation.value() == ref.generation;
    }
    case ResourceKind::Accelerator: {
      const auto it = state.accelerators.find(ref.id.str());
      return it != state.accelerators.end() && !it->second.retired &&
             it->second.generation.value() == ref.generation;
    }
    case ResourceKind::MemoryDomain: {
      const auto it = state.memory_domains.find(ref.id.str());
      return it != state.memory_domains.end() && !it->second.retired &&
             it->second.generation.value() == ref.generation;
    }
    case ResourceKind::CoherenceDomain: {
      const auto it = state.coherence_domains.find(ref.id.str());
      return it != state.coherence_domains.end() && !it->second.retired &&
             it->second.generation.value() == ref.generation;
    }
    case ResourceKind::Node:
      return state.nodes.find(ref.id.str()) != state.nodes.end();
    case ResourceKind::Unknown:
      return false;
  }
  return false;
}

TopologyGeneration bump_topology(State& state, std::string reason) {
  state.topology_generation = state.topology_generation.next();
  state.topology.generation = state.topology_generation;
  state.topology.created_at_ns = monotonic_now_ns();
  state.topology.loaded_from_state = false;
  StaleEvidenceRecord record;
  record.kind = StaleEvidenceRecord::Kind::TopologySuperseded;
  record.observed_at_ns = state.topology.created_at_ns;
  record.detail = "topology generation advanced: " + std::move(reason);
  record_stale(state, std::move(record));
  return state.topology_generation;
}

AttributionResult attribute_observation(const State& state, const Observation& observation) {
  const AttributionPolicy& policy = state.options.attribution_policy;

  AttributionResult result;
  result.event_type = observation.type;
  result.precision = observation.precision;
  result.provenance = observation.provenance;
  result.reality = reality_of(observation.provenance);
  result.bindings.coordinator_epoch = state.coordinator_epoch;
  result.bindings.observation_epoch = state.observation_epoch;
  result.bindings.topology_generation = observation.topology_generation;
  result.bindings.evidence_generation = observation.evidence_generation;
  if (observation.source.has_value()) {
    result.bindings.source_device_generation =
        DeviceGeneration{observation.source->generation};
  }
  if (observation.target.has_value()) {
    result.bindings.target_device_generation =
        DeviceGeneration{observation.target->generation};
  }
  if (observation.region_generation.has_value()) {
    result.bindings.region_generation = *observation.region_generation;
    result.bindings.region_generation_bound = true;
  }
  if (observation.region.has_value()) {
    result.region = *observation.region;
  }
  if (observation.region_generation.has_value()) {
    result.region_generation = *observation.region_generation;
  }

  // A class binding is always available: the event class itself is known.
  add_target(result, AttributionTargetKind::EventClass, std::string(to_string(observation.type)),
             0);

  // ---- Authority and currentness ---------------------------------------
  if (policy.require_current_coordinator_epoch &&
      observation.coordinator_epoch != state.coordinator_epoch) {
    add_reason(result, attribution_reason::kStaleCoordinatorEpoch);
    result.outcome = AttributionOutcome::StaleEvidence;
    return result;
  }

  const auto publisher_it = state.publishers.find(observation.source_publisher.str());
  if (publisher_it == state.publishers.end()) {
    add_reason(result, attribution_reason::kUnknownPublisher);
    result.outcome = AttributionOutcome::Unattributed;
    return result;
  }
  const PublisherState& publisher = publisher_it->second;
  if (publisher.view.boot != observation.publisher_boot) {
    add_reason(result, attribution_reason::kStalePublisher);
    result.outcome = AttributionOutcome::StaleEvidence;
    return result;
  }
  if (policy.require_current_publisher && (!publisher.view.current || publisher.view.fenced)) {
    add_reason(result, attribution_reason::kStalePublisher);
    result.outcome = AttributionOutcome::StaleEvidence;
    return result;
  }

  if (policy.require_current_topology &&
      observation.topology_generation != state.topology_generation) {
    add_reason(result, attribution_reason::kStaleTopology);
    result.outcome = AttributionOutcome::StaleEvidence;
    return result;
  }

  // ---- Region binding ---------------------------------------------------
  bool region_bound = false;
  if (observation.region.has_value()) {
    const auto region_it = state.regions.find(observation.region->str());
    if (region_it == state.regions.end()) {
      add_reason(result, attribution_reason::kUnknownResource);
    } else if (region_it->second.retired) {
      add_reason(result, attribution_reason::kRetiredRegion);
      result.outcome = AttributionOutcome::StaleEvidence;
      return result;
    } else if (policy.require_current_region_generation &&
               (!observation.region_generation.has_value() ||
                region_it->second.generation != *observation.region_generation)) {
      add_reason(result, attribution_reason::kStaleRegionGeneration);
      result.outcome = AttributionOutcome::StaleEvidence;
      return result;
    } else {
      region_bound = true;
      const RegionRecord& record = region_it->second;
      result.bindings.region_generation = record.generation;
      result.bindings.region_generation_bound = true;
      add_target(result, AttributionTargetKind::Region, record.id.str(), record.generation.value());
      if (!record.memory_domain.empty()) {
        add_target(result, AttributionTargetKind::MemoryDomain, record.memory_domain.str(),
                   record.memory_domain_generation.value());
        result.bindings.source_domain_generation = record.memory_domain_generation;
      }
      if (!record.coherence_domain.empty()) {
        add_target(result, AttributionTargetKind::CoherenceDomain, record.coherence_domain.str(),
                   0);
      }
      if (!record.workload.empty()) {
        add_target(result, AttributionTargetKind::Workload, record.workload.str(), 0);
      }
    }
  }

  // ---- Capability gating ------------------------------------------------
  const bool real_provenance = is_real_provenance(observation.provenance);
  if (real_provenance && observation.granularity == EvidenceGranularity::CacheLine &&
      capability_status(state, kCapabilityCacheLineOwnership) != CapabilityStatus::Real) {
    add_reason(result, attribution_reason::kUnsupportedCapability);
    result.outcome = AttributionOutcome::Unsupported;
    return result;
  }
  if (real_provenance && observation.locality_declared &&
      is_cxl_class_locality(observation.locality) &&
      capability_status(state, kCapabilityCxlTelemetry) != CapabilityStatus::Real) {
    add_reason(result, attribution_reason::kUnsupportedCapability);
    result.outcome = AttributionOutcome::Unsupported;
    return result;
  }

  // ---- Resource and workload bindings -----------------------------------
  if (observation.source.has_value()) {
    add_resource_target(result, *observation.source);
    if (!resource_is_current(state, *observation.source)) {
      add_reason(result, attribution_reason::kStaleRegionGeneration);
      result.outcome = AttributionOutcome::StaleEvidence;
      return result;
    }
    result.bindings.source_device_generation =
        DeviceGeneration{observation.source->generation};
  }
  if (observation.target.has_value()) {
    add_resource_target(result, *observation.target);
    if (!resource_is_current(state, *observation.target)) {
      add_reason(result, attribution_reason::kStaleRegionGeneration);
      result.outcome = AttributionOutcome::StaleEvidence;
      return result;
    }
    result.bindings.target_device_generation =
        DeviceGeneration{observation.target->generation};
  }
  if (observation.workload.has_value()) {
    add_target(result, AttributionTargetKind::Workload, observation.workload->str(), 0);
  }
  if (observation.process.has_value()) {
    add_target(result, AttributionTargetKind::Process, observation.process->str(), 0);
  }
  if (observation.coherence_domain.has_value()) {
    const auto domain_it = state.coherence_domains.find(observation.coherence_domain->str());
    if (domain_it == state.coherence_domains.end()) {
      add_reason(result, attribution_reason::kUnknownResource);
    } else if (domain_it->second.retired) {
      add_reason(result, attribution_reason::kRetiredRegion);
      result.outcome = AttributionOutcome::StaleEvidence;
      return result;
    } else if (observation.coherence_domain_generation.has_value() &&
               domain_it->second.generation != *observation.coherence_domain_generation) {
      add_reason(result, attribution_reason::kStaleRegionGeneration);
      result.outcome = AttributionOutcome::StaleEvidence;
      return result;
    } else {
      add_target(result, AttributionTargetKind::CoherenceDomain,
                 domain_it->second.id.str(), domain_it->second.generation.value());
    }
  }

  // ---- Locality ---------------------------------------------------------
  const Locality declared =
      observation.locality_declared ? observation.locality : Locality::Unknown;
  Locality derived = Locality::Unknown;
  const bool both_ends = observation.source.has_value() && observation.target.has_value();
  if (both_ends && policy.derive_locality_from_topology) {
    derived = state.topology.locality_between(*observation.source, *observation.target);
    if (derived != Locality::Unknown) {
      add_target(result, AttributionTargetKind::TopologyEdge,
                 observation.source->id.str() + "->" + observation.target->id.str(), 0);
      add_reason(result, attribution_reason::kBoundToEdge);
    }
  }
  if (declared != Locality::Unknown && derived != Locality::Unknown && declared != derived) {
    add_reason(result, attribution_reason::kConflictingLocality);
    result.candidates.push_back(std::string("declared:") + std::string(to_string(declared)));
    result.candidates.push_back(std::string("topology:") + std::string(to_string(derived)));
    result.candidate_count = 2;
  } else {
    const Locality effective = declared != Locality::Unknown ? declared : derived;
    if (effective != Locality::Unknown) {
      result.locality = effective;
      result.locality_established = true;
      add_target(result, AttributionTargetKind::LocalityClass, std::string(to_string(effective)),
                 0);
      add_reason(result, attribution_reason::kBoundToLocality);
    }
  }

  // ---- Ambiguity: a region-level claim with several candidate regions ----
  const bool claims_region_level =
      granularity_rank(observation.granularity) >= granularity_rank(EvidenceGranularity::Region);
  if (!region_bound && claims_region_level && observation.workload.has_value()) {
    std::vector<std::string> candidates = regions_for_workload(state, *observation.workload);
    if (candidates.size() > 1) {
      add_reason(result, attribution_reason::kMultipleCandidates);
      result.candidates = std::move(candidates);
      result.candidate_count = result.candidates.size();
      result.outcome = AttributionOutcome::Ambiguous;
      std::sort(result.reasons.begin(), result.reasons.end());
      add_evidence(result, observation);
      return result;
    }
  }

  // ---- Outcome ----------------------------------------------------------
  if (region_bound) {
    if (claims_region_level) {
      add_reason(result, attribution_reason::kBoundToRegionGeneration);
      result.outcome = AttributionOutcome::AttributedExact;
    } else {
      add_reason(result, attribution_reason::kCoarseGranularity);
      result.outcome = AttributionOutcome::AttributedAggregateOnly;
      // The region claim is not supported by this granularity: drop it.
      result.targets.erase(
          std::remove_if(result.targets.begin(), result.targets.end(),
                         [](const AttributionTarget& target) {
                           return target.kind == AttributionTargetKind::Region;
                         }),
          result.targets.end());
      result.region = MemoryRegionId{};
      result.region_generation = MemoryRegionGeneration{};
      result.bindings.region_generation_bound = false;
      result.precision = weakest(result.precision, Precision::AggregatedCounter);
    }
  } else if (binds_any_subject(result)) {
    if (!observation.region.has_value()) {
      add_reason(result, attribution_reason::kNoRegionIdentified);
    }
    result.outcome = AttributionOutcome::AttributedPartial;
  } else {
    add_reason(result, attribution_reason::kNoSubject);
    result.outcome = AttributionOutcome::Unattributed;
  }

  if (observation.counter_delta.has_value() &&
      observation.precision != Precision::ExactCounterDelta &&
      precision_rank(observation.precision) > precision_rank(Precision::ExactCounterDelta)) {
    add_reason(result, attribution_reason::kCounterScopeCoarser);
    result.precision = weakest(result.precision, Precision::ExactCounterDelta);
  }

  std::sort(result.reasons.begin(), result.reasons.end());
  result.reasons.erase(std::unique(result.reasons.begin(), result.reasons.end()),
                       result.reasons.end());
  add_evidence(result, observation);
  return result;
}

AttributionResult attribute_region_state(const State& state, const MemoryRegionId& region) {
  AttributionResult result;
  result.region = region;
  result.bindings.coordinator_epoch = state.coordinator_epoch;
  result.bindings.observation_epoch = state.observation_epoch;
  result.bindings.topology_generation = state.topology_generation;
  add_target(result, AttributionTargetKind::Region, region.str(), 0);

  const auto it = state.regions.find(region.str());
  if (it == state.regions.end()) {
    add_reason(result, attribution_reason::kUnknownResource);
    result.outcome = AttributionOutcome::Unattributed;
    return result;
  }
  const RegionRecord& record = it->second;
  result.bindings.region_generation = record.generation;
  result.bindings.region_generation_bound = true;
  result.targets.clear();
  add_target(result, AttributionTargetKind::Region, record.id.str(), record.generation.value());
  if (!record.memory_domain.empty()) {
    add_target(result, AttributionTargetKind::MemoryDomain, record.memory_domain.str(),
               record.memory_domain_generation.value());
  }
  if (!record.coherence_domain.empty()) {
    add_target(result, AttributionTargetKind::CoherenceDomain, record.coherence_domain.str(), 0);
  }
  if (!record.workload.empty()) {
    add_target(result, AttributionTargetKind::Workload, record.workload.str(), 0);
  }
  add_target(result, AttributionTargetKind::EventClass, "AGGREGATE", 0);

  if (record.retired) {
    add_reason(result, attribution_reason::kRetiredRegion);
    result.outcome = AttributionOutcome::StaleEvidence;
    return result;
  }

  const AggregateValue* bucket =
      state.aggregates.find(AggregateKey{AggregateDimension::Region, region.str()});
  if (bucket == nullptr || bucket->observations == 0) {
    add_reason(result, attribution_reason::kNoSubject);
    result.outcome = AttributionOutcome::Unattributed;
    return result;
  }

  result.precision = bucket->weakest_precision;
  result.provenance = Provenance::DerivedAggregation;
  result.reality = bucket->reality();

  const auto evidence_it = state.region_evidence.find(region.str());
  bool has_region_granularity = false;
  if (evidence_it != state.region_evidence.end()) {
    for (const OwnershipSample& sample : evidence_it->second.samples) {
      if (granularity_rank(sample.granularity) >= granularity_rank(EvidenceGranularity::Region)) {
        has_region_granularity = true;
        break;
      }
    }
  }

  CostInputs inputs;
  inputs.bytes = bucket->bytes;
  inputs.remote_reads = bucket->remote_reads;
  inputs.remote_writes = bucket->remote_writes;
  inputs.invalidations = bucket->invalidations;
  inputs.ownership_transfers = bucket->ownership_transfers;
  inputs.retries = bucket->retries;
  inputs.conflicts = bucket->conflicts;
  inputs.precision = result.precision;
  inputs.locality_known = false;
  result.cost = estimate_cost(state.options.cost_model, inputs);

  if (has_region_granularity) {
    add_reason(result, attribution_reason::kBoundToRegionGeneration);
    result.outcome = AttributionOutcome::AttributedExact;
  } else {
    add_reason(result, attribution_reason::kCoarseGranularity);
    result.outcome = AttributionOutcome::AttributedAggregateOnly;
  }
  std::sort(result.reasons.begin(), result.reasons.end());
  return result;
}

double locality_multiplier(const CostModel& model, Locality locality) noexcept {
  switch (locality) {
    case Locality::CxlAttached:
      return model.cxl_multiplier;
    case Locality::PooledMemory:
      return model.pooled_multiplier;
    case Locality::RemoteNode:
      return model.remote_node_multiplier;
    default:
      return 1.0;
  }
}

CostEstimate estimate_cost(const CostModel& model, const CostInputs& inputs) {
  CostEstimate cost;
  cost.precision = inputs.precision;
  cost.bytes = inputs.bytes;
  cost.remote_accesses = inputs.remote_reads + inputs.remote_writes;
  cost.ownership_transfers = inputs.ownership_transfers;
  cost.invalidations = inputs.invalidations;
  cost.retries = inputs.retries;

  const double multiplier =
      inputs.locality_known ? locality_multiplier(model, inputs.locality) : 1.0;

  // Exact facts first: they are reported regardless of the latency model.
  if (inputs.bytes != 0) {
    CostTerm term;
    term.name = "transfer.bytes";
    term.dimension = CostDimension::BytesTransferred;
    term.value = static_cast<double>(inputs.bytes);
    term.kind = inputs.measured_duration_ns >= 0 ? CostKind::Measured : CostKind::CounterDerived;
    term.coefficient = 1.0;
    term.quantity = static_cast<double>(inputs.bytes);
    term.unit = "bytes";
    cost.terms.push_back(std::move(term));
  }
  if (inputs.remote_reads != 0) {
    CostTerm term;
    term.name = "remote_read.count";
    term.dimension = CostDimension::RemoteMemoryAccesses;
    term.value = static_cast<double>(inputs.remote_reads);
    term.kind = CostKind::CounterDerived;
    term.coefficient = 1.0;
    term.quantity = static_cast<double>(inputs.remote_reads);
    term.unit = "accesses";
    cost.terms.push_back(std::move(term));
  }
  if (inputs.remote_writes != 0) {
    CostTerm term;
    term.name = "remote_write.count";
    term.dimension = CostDimension::RemoteMemoryAccesses;
    term.value = static_cast<double>(inputs.remote_writes);
    term.kind = CostKind::CounterDerived;
    term.coefficient = 1.0;
    term.quantity = static_cast<double>(inputs.remote_writes);
    term.unit = "accesses";
    cost.terms.push_back(std::move(term));
  }
  if (inputs.invalidations != 0) {
    CostTerm term;
    term.name = "invalidation.count";
    term.dimension = CostDimension::InvalidatedBytes;
    term.value = static_cast<double>(inputs.invalidations);
    term.kind = CostKind::CounterDerived;
    term.coefficient = 1.0;
    term.quantity = static_cast<double>(inputs.invalidations);
    term.unit = "invalidations";
    cost.terms.push_back(std::move(term));
  }
  if (inputs.ownership_transfers != 0) {
    CostTerm term;
    term.name = "ownership_transfer.count";
    term.dimension = CostDimension::OwnershipTransfers;
    term.value = static_cast<double>(inputs.ownership_transfers);
    term.kind = CostKind::CounterDerived;
    term.coefficient = 1.0;
    term.quantity = static_cast<double>(inputs.ownership_transfers);
    term.unit = "transfers";
    cost.terms.push_back(std::move(term));
  }
  if (inputs.retries != 0) {
    CostTerm term;
    term.name = "retry.count";
    term.dimension = CostDimension::RetryOverhead;
    term.value = static_cast<double>(inputs.retries);
    term.kind = CostKind::CounterDerived;
    term.coefficient = 1.0;
    term.quantity = static_cast<double>(inputs.retries);
    term.unit = "retries";
    cost.terms.push_back(std::move(term));
  }

  if (inputs.measured_duration_ns >= 0) {
    cost.kind = CostKind::Measured;
    cost.latency_ns = static_cast<double>(inputs.measured_duration_ns);
    CostTerm term;
    term.name = "measured.duration";
    term.dimension = CostDimension::LatencyNanos;
    term.value = cost.latency_ns;
    term.kind = CostKind::Measured;
    term.coefficient = 1.0;
    term.quantity = 1.0;
    term.unit = "ns";
    cost.terms.push_back(std::move(term));
  } else if (!model.defined) {
    cost.kind = cost.terms.empty() ? CostKind::Unknown : CostKind::CounterDerived;
  } else {
    cost.kind = CostKind::ModelEstimated;
    cost.model_id = model.id;
    cost.model_version = model.version;
    cost.bandwidth_bytes_per_ns = model.bandwidth_bytes_per_ns;

    auto model_term = [&cost, &multiplier](std::string name, CostDimension dimension,
                                           double coefficient, double quantity,
                                           std::string unit) {
      if (quantity == 0.0) {
        return;
      }
      CostTerm term;
      term.name = std::move(name);
      term.dimension = dimension;
      term.coefficient = coefficient * multiplier;
      term.quantity = quantity;
      term.value = term.coefficient * quantity;
      term.kind = CostKind::ModelEstimated;
      term.unit = std::move(unit);
      cost.latency_ns += term.value;
      cost.terms.push_back(std::move(term));
    };

    model_term("remote_read.latency", CostDimension::LatencyNanos, model.remote_read_latency_ns,
               static_cast<double>(inputs.remote_reads), "ns");
    model_term("remote_write.latency", CostDimension::LatencyNanos, model.remote_write_latency_ns,
               static_cast<double>(inputs.remote_writes), "ns");
    model_term("invalidation.latency", CostDimension::LatencyNanos,
               model.invalidation_latency_ns, static_cast<double>(inputs.invalidations), "ns");
    model_term("ownership_transfer.latency", CostDimension::LatencyNanos,
               model.ownership_transfer_latency_ns,
               static_cast<double>(inputs.ownership_transfers), "ns");
    model_term("retry.latency", CostDimension::LatencyNanos, model.retry_latency_ns,
               static_cast<double>(inputs.retries), "ns");
    model_term("conflict.stall", CostDimension::StallCycles, model.conflict_stall_ns,
               static_cast<double>(inputs.conflicts), "ns");
    if (model.bandwidth_bytes_per_ns > 0.0 && inputs.bytes != 0) {
      CostTerm term;
      term.name = "transfer.bandwidth_time";
      term.dimension = CostDimension::LatencyNanos;
      term.coefficient = 1.0 / model.bandwidth_bytes_per_ns;
      term.quantity = static_cast<double>(inputs.bytes);
      term.value = term.coefficient * term.quantity;
      term.kind = CostKind::ModelEstimated;
      term.unit = "ns";
      cost.latency_ns += term.value;
      cost.terms.push_back(std::move(term));
    }
  }

  if (!(cost.latency_ns == cost.latency_ns) || cost.latency_ns < 0.0) {
    cost.latency_ns = 0.0;
    cost.lower_bound = true;
  }

  std::sort(cost.terms.begin(), cost.terms.end(),
            [](const CostTerm& a, const CostTerm& b) { return a.name < b.name; });
  return cost;
}

}  // namespace sol::coherence::detail

namespace sol::coherence {
namespace {

constexpr std::array<std::string_view, 7> kOutcomeNames = {
    "ATTRIBUTED_EXACT", "ATTRIBUTED_PARTIAL", "ATTRIBUTED_AGGREGATE_ONLY", "AMBIGUOUS",
    "UNATTRIBUTED",     "STALE_EVIDENCE",     "UNSUPPORTED"};

constexpr std::array<std::string_view, 9> kTargetKindNames = {
    "RESOURCE", "MEMORY_DOMAIN", "COHERENCE_DOMAIN", "REGION",         "WORKLOAD",
    "PROCESS",  "TOPOLOGY_EDGE", "LOCALITY_CLASS",   "EVENT_CLASS"};

}  // namespace

std::string_view to_string(AttributionOutcome outcome) noexcept {
  const auto index = static_cast<std::size_t>(outcome);
  return index < kOutcomeNames.size() ? kOutcomeNames[index] : std::string_view("UNATTRIBUTED");
}

Result<AttributionOutcome> parse_attribution_outcome(std::string_view text) {
  for (std::size_t i = 0; i < kOutcomeNames.size(); ++i) {
    if (detail::iequals(kOutcomeNames[i], text)) {
      return Result<AttributionOutcome>(static_cast<AttributionOutcome>(i));
    }
  }
  return fail_as<AttributionOutcome>(ErrorCode::InvalidArgument, "unknown attribution outcome",
                                     std::string(text));
}

std::string_view to_string(AttributionTargetKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  return index < kTargetKindNames.size() ? kTargetKindNames[index]
                                         : std::string_view("RESOURCE");
}

bool AttributionResult::has_reason(std::string_view reason) const noexcept {
  for (const std::string& existing : reasons) {
    if (existing == reason) {
      return true;
    }
  }
  return false;
}

}  // namespace sol::coherence
