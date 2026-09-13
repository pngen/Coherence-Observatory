// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// The runtime facade.
//
// Locking: exactly one mutex (Impl::mutex) guards every field of State.  No
// lock is ever held across socket I/O, filesystem I/O, callbacks, vendor APIs,
// long-running analysis, thread joins or process waits.  Nested locking does
// not occur anywhere in this file; analysis and snapshot assembly run while
// the state mutex is held but never acquire another lock.

#include "coherence/observatory.hpp"

#include <algorithm>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "coherence/checked.hpp"
#include "detail/cost.hpp"
#include "detail/persistence.hpp"
#include "detail/state.hpp"
#include "text_util.hpp"

namespace sol::coherence {
namespace {

using detail::CounterKey;
using detail::CounterState;
using detail::JournalEntry;
using detail::PublisherState;
using detail::PublisherWatermark;
using detail::State;

/// Deterministic event identity for counter-derived evidence.
CoherenceEventId counter_event_id(const std::string& counter, EventSequence sequence) {
  const std::uint64_t h = detail::fnv1a_str(detail::fnv1a_init(), counter);
  return CoherenceEventId{detail::fnv1a_u64(h, sequence.value())};
}

bool valid_enum_range(std::uint8_t value, std::uint8_t exclusive_max) noexcept {
  return static_cast<std::uint8_t>(value) < exclusive_max;
}

struct SequenceDecision {
  IngestionDisposition disposition = IngestionDisposition::Rejected;
  ErrorCode code = ErrorCode::Ok;
  std::string detail;
  std::uint64_t missing = 0;
  bool mutate = false;
};

constexpr std::string_view kDefaultCacheLineCapability = "coherence.cache_line_ownership";
constexpr std::string_view kDefaultInvalidationCapability = "coherence.invalidation_counters";
constexpr std::string_view kDefaultAcceleratorCoherenceCapability =
    "accelerator.coherence_telemetry";
constexpr std::string_view kDefaultCxlCapability = "memory.cxl_telemetry";
constexpr std::string_view kDefaultPooledCapability = "memory.pooled_telemetry";

}  // namespace

std::string_view to_string(IngestionDisposition disposition) noexcept {
  switch (disposition) {
    case IngestionDisposition::Accepted: return "ACCEPTED";
    case IngestionDisposition::AcceptedLate: return "ACCEPTED_LATE";
    case IngestionDisposition::Duplicate: return "DUPLICATE";
    case IngestionDisposition::Rejected: return "REJECTED";
  }
  return "REJECTED";
}

namespace detail {
namespace {

/// Applies per-publisher sequence semantics.
///
/// Deterministic outcomes:
///   * first sequence            -> accepted
///   * strictly increasing       -> accepted, gaps recorded explicitly
///   * inside the window, unseen -> accepted late (out of order)
///   * inside the window, seen   -> duplicate, or conflicting duplicate
///   * below the window          -> rejected as a stale replay
SequenceDecision account_sequence(PublisherState& publisher, std::uint64_t sequence,
                                  std::uint64_t content_hash) {
  SequenceDecision decision;
  const std::uint64_t window = Limits::kMaxSequenceWindow;

  if (!publisher.has_watermark) {
    decision.disposition = IngestionDisposition::Accepted;
    decision.mutate = true;
    return decision;
  }

  // A watermark restored from durable state cannot be distinguished from a
  // replay inside the retention window, so anything at or below it is refused.
  if (publisher.durable_watermark && sequence <= publisher.high_watermark) {
    decision.disposition = IngestionDisposition::Rejected;
    decision.code = ErrorCode::StaleSequence;
    decision.detail = "sequence at or below the durable replay watermark";
    return decision;
  }

  if (sequence > publisher.high_watermark) {
    decision.disposition = IngestionDisposition::Accepted;
    decision.missing = sequence - publisher.high_watermark - 1;
    decision.mutate = true;
    if (decision.missing != 0) {
      decision.detail = "sequence gap of " + detail::format_u64(decision.missing);
    }
    return decision;
  }

  const auto seen = publisher.window.find(sequence);
  if (seen != publisher.window.end()) {
    if (seen->second == content_hash) {
      decision.disposition = IngestionDisposition::Duplicate;
      decision.code = ErrorCode::Duplicate;
      decision.detail = "duplicate observation";
      return decision;
    }
    decision.disposition = IngestionDisposition::Rejected;
    decision.code = ErrorCode::Conflict;
    decision.detail = "conflicting duplicate: same sequence, different content";
    return decision;
  }

  if (publisher.high_watermark >= window && sequence <= publisher.high_watermark - window) {
    decision.disposition = IngestionDisposition::Rejected;
    decision.code = ErrorCode::StaleSequence;
    decision.detail = "sequence below the retention window";
    return decision;
  }

  decision.disposition = IngestionDisposition::AcceptedLate;
  decision.mutate = true;
  return decision;
}

void commit_sequence(PublisherState& publisher, std::uint64_t sequence,
                     std::uint64_t content_hash) {
  if (!publisher.has_watermark || sequence > publisher.high_watermark) {
    publisher.high_watermark = sequence;
    publisher.has_watermark = true;
  }
  if (publisher.window.find(sequence) == publisher.window.end()) {
    publisher.window.emplace(sequence, content_hash);
    publisher.window_order.push_back(sequence);
  }
  while (publisher.window_order.size() > Limits::kMaxSequenceWindow) {
    const std::uint64_t victim = publisher.window_order.front();
    publisher.window_order.pop_front();
    publisher.window.erase(victim);
  }
}

void commit_event_id(PublisherState& publisher, std::uint64_t event_id,
                     std::uint64_t content_hash) {
  if (publisher.recent_event_ids.find(event_id) == publisher.recent_event_ids.end()) {
    publisher.recent_event_ids.emplace(event_id, content_hash);
    publisher.recent_event_order.push_back(event_id);
  }
  while (publisher.recent_event_order.size() > Limits::kMaxRecentEventIds) {
    const std::uint64_t victim = publisher.recent_event_order.front();
    publisher.recent_event_order.pop_front();
    publisher.recent_event_ids.erase(victim);
  }
}

}  // namespace
}  // namespace detail

struct Observatory::Impl {
  mutable std::mutex mutex;
  detail::State state;

  Impl() {
    state.started_at_ns = monotonic_now_ns();
    state.topology.generation = state.topology_generation;
    state.topology.created_at_ns = state.started_at_ns;
  }
};

namespace {

void install_default_capabilities(State& state) {
  auto install = [&state](std::string key, CapabilityStatus status, std::string detail_text,
                          std::string mechanism) {
    Capability capability;
    capability.key = std::move(key);
    capability.status = status;
    capability.detail = std::move(detail_text);
    capability.mechanism = std::move(mechanism);
    state.capabilities[capability.key] = std::move(capability);
  };
  install(std::string(kDefaultCacheLineCapability), CapabilityStatus::Unsupported,
          "no operating-system or vendor interface on this host reports per-cache-line "
          "ownership or coherent sharer lists",
          "none");
  install(std::string(kDefaultInvalidationCapability), CapabilityStatus::Unsupported,
          "no interface on this host reports coherence invalidation counts; aggregate cache "
          "counters do not identify invalidations",
          "none");
  install(std::string(kDefaultAcceleratorCoherenceCapability), CapabilityStatus::Unsupported,
          "no vendor interface on this host exposes accelerator cache-line ownership or "
          "peer-coherence transitions",
          "none");
  install(std::string(kDefaultCxlCapability), CapabilityStatus::Unsupported,
          "no CXL device or CXL telemetry interface was detected on this host",
          "none");
  install(std::string(kDefaultPooledCapability), CapabilityStatus::Unsupported,
          "no pooled-memory fabric telemetry interface was detected on this host",
          "none");
}

Status validate_generation_rollback(std::uint64_t existing, std::uint64_t incoming,
                                    const char* what) {
  if (incoming < existing) {
    return fail(ErrorCode::StaleGeneration,
                std::string(what) + " generation would roll back",
                std::string("existing=") + detail::format_u64(existing) +
                    " incoming=" + detail::format_u64(incoming));
  }
  return Status();
}

bool is_power_of_two(std::uint64_t value) noexcept {
  return value != 0 && (value & (value - 1)) == 0;
}

}  // namespace

Observatory::Observatory(ObservatoryOptions options) : impl_(std::make_unique<Impl>()) {
  State& state = impl_->state;
  state.options = std::move(options);
  if (state.options.observer_id.empty()) {
    state.options.observer_id = ObserverId{"observer.local"};
  }
  if (state.options.node_id.empty()) {
    state.options.node_id = NodeId{"node.local"};
  }
  if (state.options.initial_coordinator_epoch.is_zero()) {
    state.options.initial_coordinator_epoch = CoordinatorEpoch{1};
  }
  state.coordinator_epoch = state.options.initial_coordinator_epoch;
  if (!state.options.cost_model.defined) {
    state.options.cost_model = default_cost_model();
  }
  install_default_capabilities(state);
  NodeRecord node;
  node.id = state.options.node_id;
  node.display_name = state.options.display_name.empty() ? state.options.node_id.str()
                                                         : state.options.display_name;
  node.registered_at_ns = state.started_at_ns;
  state.nodes[node.id.str()] = std::move(node);
}

Observatory::~Observatory() = default;

// ---- Epochs ------------------------------------------------------------

EpochSet Observatory::epochs() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  EpochSet set;
  set.coordinator_epoch = impl_->state.coordinator_epoch;
  set.observation_epoch = impl_->state.observation_epoch;
  set.topology_generation = impl_->state.topology_generation;
  set.snapshot_generation = impl_->state.snapshot_generation;
  return set;
}

CoordinatorEpoch Observatory::coordinator_epoch() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state.coordinator_epoch;
}

ObservationEpoch Observatory::observation_epoch() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state.observation_epoch;
}

TopologyGeneration Observatory::topology_generation() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state.topology_generation;
}

// ---- Structural registration -------------------------------------------

Status Observatory::register_node(NodeRecord record) {
  const Status token = validate_identity_token(record.id.view());
  if (!token.ok()) {
    return token;
  }
  if (record.display_name.size() > Limits::kMaxRegionAnnotationLength) {
    return fail(ErrorCode::TooLarge, "node display name too long");
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  State& state = impl_->state;
  if (state.nodes.find(record.id.str()) == state.nodes.end() &&
      state.nodes.size() >= Limits::kMaxNodeCount) {
    return fail(ErrorCode::TooMany, "node capacity reached");
  }
  if (record.registered_at_ns == 0) {
    record.registered_at_ns = monotonic_now_ns();
  }
  state.nodes[record.id.str()] = std::move(record);
  return Status();
}

Status Observatory::register_processor(ProcessorRecord record) {
  const Status token = validate_identity_token(record.id.view());
  if (!token.ok()) {
    return token;
  }
  if (record.generation.is_zero()) {
    return fail(ErrorCode::InvalidArgument, "processor generation must be non-zero");
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  State& state = impl_->state;
  const auto existing = state.processors.find(record.id.str());
  if (existing == state.processors.end()) {
    if (state.processors.size() >= Limits::kMaxProcessorCount) {
      return fail(ErrorCode::TooMany, "processor capacity reached");
    }
  } else {
    const Status rollback =
        validate_generation_rollback(existing->second.generation.value(),
                                     record.generation.value(), "processor");
    if (!rollback.ok()) {
      return rollback;
    }
  }
  if (!record.local_memory_domain.empty()) {
    const auto domain = state.memory_domains.find(record.local_memory_domain.str());
    if (domain == state.memory_domains.end()) {
      return fail(ErrorCode::InvalidArgument, "processor references an unregistered memory domain",
                  record.local_memory_domain.str());
    }
  }
  if (record.registered_at_ns == 0) {
    record.registered_at_ns = monotonic_now_ns();
  }
  state.processors[record.id.str()] = std::move(record);
  return Status();
}

Status Observatory::register_accelerator(AcceleratorRecord record) {
  const Status token = validate_identity_token(record.id.view());
  if (!token.ok()) {
    return token;
  }
  if (record.generation.is_zero()) {
    return fail(ErrorCode::InvalidArgument, "accelerator generation must be non-zero");
  }
  if (record.vendor.size() > Limits::kMaxNameLength ||
      record.vendor_uuid.size() > Limits::kMaxRegionAnnotationLength) {
    return fail(ErrorCode::TooLarge, "accelerator description too long");
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  State& state = impl_->state;
  const auto existing = state.accelerators.find(record.id.str());
  if (existing == state.accelerators.end()) {
    if (state.accelerators.size() >= Limits::kMaxAcceleratorCount) {
      return fail(ErrorCode::TooMany, "accelerator capacity reached");
    }
  } else {
    const Status rollback =
        validate_generation_rollback(existing->second.generation.value(),
                                     record.generation.value(), "accelerator");
    if (!rollback.ok()) {
      return rollback;
    }
    if (existing->second.vendor_uuid != record.vendor_uuid && !record.vendor_uuid.empty() &&
        !existing->second.vendor_uuid.empty()) {
      return fail(ErrorCode::Conflict,
                  "accelerator identity would change meaning without a generation change",
                  record.id.str());
    }
  }
  if (!record.local_memory_domain.empty()) {
    const auto domain = state.memory_domains.find(record.local_memory_domain.str());
    if (domain == state.memory_domains.end()) {
      return fail(ErrorCode::InvalidArgument,
                  "accelerator references an unregistered memory domain",
                  record.local_memory_domain.str());
    }
  }
  if (record.registered_at_ns == 0) {
    record.registered_at_ns = monotonic_now_ns();
  }
  state.accelerators[record.id.str()] = std::move(record);
  return Status();
}

Status Observatory::register_memory_domain(MemoryDomainRecord record) {
  const Status token = validate_identity_token(record.id.view());
  if (!token.ok()) {
    return token;
  }
  if (record.generation.is_zero()) {
    return fail(ErrorCode::InvalidArgument, "memory domain generation must be non-zero");
  }
  if (!valid_enum_range(static_cast<std::uint8_t>(record.kind), 7)) {
    return fail(ErrorCode::InvalidArgument, "memory domain kind out of range");
  }
  if (record.capacity_known && record.capacity_bytes > Limits::kMaxByteCount) {
    return fail(ErrorCode::OutOfRange, "memory domain capacity out of range");
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  State& state = impl_->state;
  const auto existing = state.memory_domains.find(record.id.str());
  if (existing == state.memory_domains.end()) {
    if (state.memory_domains.size() >= Limits::kMaxMemoryDomainCount) {
      return fail(ErrorCode::TooMany, "memory domain capacity reached");
    }
  } else {
    const Status rollback =
        validate_generation_rollback(existing->second.generation.value(),
                                     record.generation.value(), "memory domain");
    if (!rollback.ok()) {
      return rollback;
    }
  }
  if (!record.coherence_domain.empty()) {
    const auto domain = state.coherence_domains.find(record.coherence_domain.str());
    if (domain == state.coherence_domains.end()) {
      return fail(ErrorCode::InvalidArgument,
                  "memory domain references an unregistered coherence domain",
                  record.coherence_domain.str());
    }
  }
  if (record.registered_at_ns == 0) {
    record.registered_at_ns = monotonic_now_ns();
  }
  state.memory_domains[record.id.str()] = std::move(record);
  return Status();
}

Status Observatory::register_coherence_domain(CoherenceDomainRecord record) {
  const Status token = validate_identity_token(record.id.view());
  if (!token.ok()) {
    return token;
  }
  if (record.generation.is_zero()) {
    return fail(ErrorCode::InvalidArgument, "coherence domain generation must be non-zero");
  }
  if (record.protocol_family.size() > Limits::kMaxRegionAnnotationLength) {
    return fail(ErrorCode::TooLarge, "coherence domain protocol family too long");
  }
  if (record.members.size() > Limits::kMaxTopologyLinkCount) {
    return fail(ErrorCode::TooMany, "too many coherence domain members");
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  State& state = impl_->state;
  const auto existing = state.coherence_domains.find(record.id.str());
  if (existing == state.coherence_domains.end()) {
    if (state.coherence_domains.size() >= Limits::kMaxCoherenceDomainCount) {
      return fail(ErrorCode::TooMany, "coherence domain capacity reached");
    }
  } else {
    const Status rollback =
        validate_generation_rollback(existing->second.generation.value(),
                                     record.generation.value(), "coherence domain");
    if (!rollback.ok()) {
      return rollback;
    }
  }
  for (const ResourceRef& member : record.members) {
    if (member.empty()) {
      return fail(ErrorCode::InvalidArgument, "coherence domain member reference is empty");
    }
    if (!detail::resource_is_current(state, member)) {
      return fail(ErrorCode::InvalidArgument,
                  "coherence domain member is not a current registered resource",
                  detail::resource_text(member));
    }
  }
  if (record.registered_at_ns == 0) {
    record.registered_at_ns = monotonic_now_ns();
  }
  state.coherence_domains[record.id.str()] = std::move(record);
  return Status();
}

Status Observatory::register_region(RegionRecord record) {
  const Status token = validate_identity_token(record.id.view());
  if (!token.ok()) {
    return token;
  }
  if (record.generation.is_zero()) {
    return fail(ErrorCode::InvalidArgument, "region generation must be non-zero");
  }
  if (!valid_enum_range(static_cast<std::uint8_t>(record.sharing_scope), 6)) {
    return fail(ErrorCode::InvalidArgument, "sharing scope out of range");
  }
  if (record.owner.size() > Limits::kMaxRegionAnnotationLength ||
      record.annotation.size() > Limits::kMaxRegionAnnotationLength) {
    return fail(ErrorCode::TooLarge, "region annotation too long");
  }
  if (record.size_known && record.size_bytes > Limits::kMaxByteCount) {
    return fail(ErrorCode::OutOfRange, "region size out of range");
  }
  if (record.page_size_known &&
      (!is_power_of_two(record.page_size_bytes) || record.page_size_bytes > (1ull << 30))) {
    return fail(ErrorCode::InvalidArgument, "region page size must be a power of two");
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  State& state = impl_->state;
  const auto existing = state.regions.find(record.id.str());
  if (existing == state.regions.end()) {
    if (state.regions.size() >= Limits::kMaxRegionCount) {
      return fail(ErrorCode::TooMany, "region capacity reached");
    }
  } else {
    const Status rollback =
        validate_generation_rollback(existing->second.generation.value(),
                                     record.generation.value(), "region");
    if (!rollback.ok()) {
      return rollback;
    }
    if (existing->second.retired && record.generation == existing->second.generation) {
      return fail(ErrorCode::RetiredEntity,
                  "region generation is retired and cannot be re-registered at the same "
                  "generation",
                  record.id.str());
    }
  }
  if (!record.memory_domain.empty()) {
    const auto domain = state.memory_domains.find(record.memory_domain.str());
    if (domain == state.memory_domains.end()) {
      return fail(ErrorCode::InvalidArgument, "region references an unregistered memory domain",
                  record.memory_domain.str());
    }
    if (record.memory_domain_generation.is_zero()) {
      record.memory_domain_generation = domain->second.generation;
    } else if (domain->second.generation != record.memory_domain_generation) {
      return fail(ErrorCode::StaleGeneration,
                  "region references a superseded memory domain generation",
                  record.memory_domain.str());
    }
  }
  if (!record.coherence_domain.empty()) {
    const auto domain = state.coherence_domains.find(record.coherence_domain.str());
    if (domain == state.coherence_domains.end()) {
      return fail(ErrorCode::InvalidArgument,
                  "region references an unregistered coherence domain",
                  record.coherence_domain.str());
    }
  }
  if (record.registered_at_ns == 0) {
    record.registered_at_ns = monotonic_now_ns();
  }
  record.loaded_from_state = false;
  state.regions[record.id.str()] = std::move(record);
  return Status();
}

Status Observatory::retire_region(const MemoryRegionId& region,
                                  MemoryRegionGeneration generation, std::string reason) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  State& state = impl_->state;
  const auto it = state.regions.find(region.str());
  if (it == state.regions.end()) {
    return fail(ErrorCode::NotFound, "unknown region", region.str());
  }
  if (it->second.generation != generation) {
    return fail(ErrorCode::StaleGeneration, "region retirement names a superseded generation",
                region.str());
  }
  if (it->second.retired) {
    return fail(ErrorCode::RetiredEntity, "region is already retired", region.str());
  }
  it->second.retired = true;
  it->second.retired_at_ns = monotonic_now_ns();
  StaleEvidenceRecord record;
  record.kind = StaleEvidenceRecord::Kind::RetiredRegion;
  record.region = region;
  record.observed_at_ns = it->second.retired_at_ns;
  record.detail = "region retired at generation " + detail::format_u64(generation.value()) +
                  ": " + std::move(reason);
  detail::record_stale(state, std::move(record));
  return Status();
}

Status Observatory::set_topology_link(TopologyLink link) {
  if (link.from.empty() || link.to.empty()) {
    return fail(ErrorCode::InvalidArgument, "topology link endpoints must not be empty");
  }
  if (link.from.kind == link.to.kind && link.from.id == link.to.id &&
      link.from.generation == link.to.generation) {
    return fail(ErrorCode::InvalidArgument, "topology link must connect two distinct resources");
  }
  if (!valid_enum_range(static_cast<std::uint8_t>(link.locality), kLocalityCount)) {
    return fail(ErrorCode::InvalidArgument, "topology link locality out of range");
  }
  if (link.latency_ns < -1 || link.latency_ns > Limits::kMaxMeasuredDurationNs) {
    return fail(ErrorCode::OutOfRange, "topology link latency out of range");
  }
  if (link.bandwidth_bytes_per_second > Limits::kMaxByteCount) {
    return fail(ErrorCode::OutOfRange, "topology link bandwidth out of range");
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  State& state = impl_->state;
  if (!detail::resource_is_current(state, link.from)) {
    return fail(ErrorCode::InvalidArgument, "topology link source is not a current resource",
                detail::resource_text(link.from));
  }
  if (!detail::resource_is_current(state, link.to)) {
    return fail(ErrorCode::InvalidArgument, "topology link target is not a current resource",
                detail::resource_text(link.to));
  }
  for (TopologyLink& existing : state.topology.links) {
    if (existing.from == link.from && existing.to == link.to) {
      existing = std::move(link);
      return Status();
    }
  }
  if (state.topology.links.size() >= Limits::kMaxTopologyLinkCount) {
    return fail(ErrorCode::TooMany, "topology link capacity reached");
  }
  state.topology.links.push_back(std::move(link));
  return Status();
}

Result<TopologyGeneration> Observatory::bump_topology_generation(std::string reason) {
  if (reason.size() > Limits::kMaxRegionAnnotationLength) {
    return fail_as<TopologyGeneration>(ErrorCode::TooLarge, "topology change reason too long");
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return Result<TopologyGeneration>(detail::bump_topology(impl_->state, std::move(reason)));
}

Status Observatory::set_capability(Capability capability) {
  if (capability.key.empty() || capability.key.size() > Limits::kMaxNameLength) {
    return fail(ErrorCode::InvalidArgument, "capability key must be a bounded non-empty token");
  }
  if (capability.detail.size() > 512 || capability.mechanism.size() > 128) {
    return fail(ErrorCode::TooLarge, "capability description too long");
  }
  if (!valid_enum_range(static_cast<std::uint8_t>(capability.status), 3)) {
    return fail(ErrorCode::InvalidArgument, "capability status out of range");
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->state.capabilities[capability.key] = std::move(capability);
  return Status();
}

Status Observatory::set_cost_model(CostModel model) {
  const Status valid = model.validate();
  if (!valid.ok()) {
    return valid;
  }
  model.defined = true;
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->state.options.cost_model = std::move(model);
  return Status();
}

CostModel Observatory::cost_model() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state.options.cost_model;
}

// ---- Publisher authority -----------------------------------------------

Result<PublisherView> Observatory::register_publisher(const PublisherRegistration& registration) {
  const Status id_token = validate_identity_token(registration.id.view());
  if (!id_token.ok()) {
    return Result<PublisherView>(id_token.error());
  }
  if (registration.boot.is_zero()) {
    return fail_as<PublisherView>(ErrorCode::InvalidArgument,
                                  "publisher boot identity must be non-zero");
  }
  if (!registration.observer.empty()) {
    const Status token = validate_identity_token(registration.observer.view());
    if (!token.ok()) {
      return Result<PublisherView>(token.error());
    }
  }
  if (!registration.node.empty()) {
    const Status token = validate_identity_token(registration.node.view());
    if (!token.ok()) {
      return Result<PublisherView>(token.error());
    }
  }
  if (registration.display_name.size() > Limits::kMaxRegionAnnotationLength) {
    return fail_as<PublisherView>(ErrorCode::TooLarge, "publisher display name too long");
  }
  if (!valid_enum_range(static_cast<std::uint8_t>(registration.provenance), 10)) {
    return fail_as<PublisherView>(ErrorCode::InvalidArgument, "publisher provenance out of range");
  }

  const std::lock_guard<std::mutex> lock(impl_->mutex);
  State& state = impl_->state;

  if (!registration.coordinator_epoch.is_zero() &&
      registration.coordinator_epoch != state.coordinator_epoch) {
    return fail_as<PublisherView>(ErrorCode::StaleEpoch,
                                  "publisher presented a stale coordinator epoch",
                                  detail::format_u64(registration.coordinator_epoch.value()));
  }

  const std::string key = registration.id.str();
  auto existing = state.publishers.find(key);
  if (existing == state.publishers.end()) {
    if (state.publishers.size() >= Limits::kMaxPublisherCount) {
      return fail_as<PublisherView>(ErrorCode::TooMany, "publisher capacity reached");
    }
  } else if (existing->second.view.boot == registration.boot) {
    if (existing->second.view.fenced) {
      return fail_as<PublisherView>(ErrorCode::StaleBoot,
                                    "a fenced publisher boot identity is never re-admitted",
                                    key);
    }
  } else {
    // A replacement boot supersedes the previous one.
    PublisherWatermark watermark;
    watermark.publisher = existing->second.view.id;
    watermark.boot = existing->second.view.boot;
    watermark.high_watermark = existing->second.high_watermark;
    watermark.has_watermark = existing->second.has_watermark;
    watermark.fenced = true;
    watermark.fence_reason = FenceReason::SupersededByNewBoot;
    watermark.boot_incarnations = existing->second.boot_incarnations;
    watermark.last_seen_ns = existing->second.view.last_seen_ns;
    if (state.watermarks.size() >= Limits::kMaxPublisherWatermarks &&
        state.watermarks.find(detail::boot_key(watermark.publisher, watermark.boot)) ==
            state.watermarks.end()) {
      return fail_as<PublisherView>(ErrorCode::TooMany,
                                    "publisher boot watermark capacity reached");
    }
    state.watermarks[detail::boot_key(watermark.publisher, watermark.boot)] = watermark;

    StaleEvidenceRecord record;
    record.kind = StaleEvidenceRecord::Kind::StaleBoot;
    record.publisher = watermark.publisher;
    record.publisher_boot = watermark.boot;
    record.observed_at_ns = monotonic_now_ns();
    record.detail = "publisher boot superseded by a replacement incarnation";
    detail::record_stale(state, std::move(record));
  }

  const std::string boot_key = detail::boot_key(registration.id, registration.boot);
  if (const auto fenced = state.watermarks.find(boot_key); fenced != state.watermarks.end()) {
    if (fenced->second.fenced) {
      return fail_as<PublisherView>(
          ErrorCode::StaleBoot,
          "publisher boot identity was fenced and can never be re-admitted", boot_key);
    }
  }

  PublisherState& publisher = state.publishers[key];
  const std::uint64_t previous_incarnations = publisher.boot_incarnations;
  const bool rebinding = publisher.view.boot != registration.boot;
  publisher.view.id = registration.id;
  publisher.view.boot = registration.boot;
  publisher.view.observer = registration.observer;
  publisher.view.node = registration.node;
  publisher.view.display_name = registration.display_name;
  publisher.view.provenance = registration.provenance;
  publisher.view.reality = reality_of(registration.provenance);
  publisher.view.registered_epoch = state.coordinator_epoch;
  publisher.view.evidence_generation = registration.evidence_generation.is_zero()
                                           ? EvidenceGeneration{1}
                                           : registration.evidence_generation;
  publisher.view.sampling_epoch = registration.sampling_epoch.is_zero() ? SamplingEpoch{1}
                                                                        : registration.sampling_epoch;
  publisher.view.registered_at_ns = monotonic_now_ns();
  publisher.view.last_seen_ns = publisher.view.registered_at_ns;
  publisher.view.current = true;
  publisher.view.fenced = false;
  publisher.view.fence_reason = FenceReason::NotFenced;
  publisher.view.fence_detail.clear();
  publisher.view.loaded_from_state = false;
  if (rebinding) {
    publisher.view.sequences = SequenceState{};
    publisher.view.accepted_events = 0;
    publisher.view.accepted_batches = 0;
    publisher.view.accepted_counters = 0;
    publisher.view.rejected_events = 0;
    publisher.has_watermark = false;
    publisher.high_watermark = 0;
    publisher.window.clear();
    publisher.window_order.clear();
    publisher.recent_event_ids.clear();
    publisher.recent_event_order.clear();
    publisher.boot_incarnations = previous_incarnations + 1;
  }

  // Restore a durable replay watermark for the same boot, if one exists.
  if (const auto restored = state.watermarks.find(boot_key);
      restored != state.watermarks.end() && restored->second.boot == registration.boot) {
    if (restored->second.has_watermark && !publisher.has_watermark) {
      publisher.has_watermark = true;
      publisher.high_watermark = restored->second.high_watermark;
      publisher.durable_watermark = true;
      publisher.view.sequences.high_watermark = EventSequence{restored->second.high_watermark};
    }
    restored->second.fenced = false;
    restored->second.fence_reason = FenceReason::NotFenced;
  }

  return Result<PublisherView>(publisher.view);
}

Result<PublisherView> Observatory::fence_publisher_boot(const PublisherId& publisher_id,
                                                         PublisherBootId boot,
                                                         FenceReason reason,
                                                         std::string detail_text) {
  if (detail_text.size() > Limits::kMaxRegionAnnotationLength) {
    return fail_as<PublisherView>(ErrorCode::TooLarge, "fence detail too long");
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  State& state = impl_->state;
  const auto it = state.publishers.find(publisher_id.str());
  if (it == state.publishers.end()) {
    return fail_as<PublisherView>(ErrorCode::NotFound, "unknown publisher", publisher_id.str());
  }
  if (it->second.view.boot != boot) {
    return fail_as<PublisherView>(ErrorCode::StaleBoot,
                                  "fence names a publisher boot that is not current",
                                  detail::format_u64(boot.value()));
  }
  PublisherState& publisher = it->second;
  publisher.view.current = false;
  publisher.view.fenced = true;
  publisher.view.fence_reason = reason;
  publisher.view.fence_detail = std::move(detail_text);

  PublisherWatermark& watermark = state.watermarks[detail::boot_key(publisher_id, boot)];
  watermark.publisher = publisher_id;
  watermark.boot = boot;
  watermark.high_watermark = publisher.high_watermark;
  watermark.has_watermark = publisher.has_watermark;
  watermark.fenced = true;
  watermark.fence_reason = reason;
  watermark.boot_incarnations = publisher.boot_incarnations;
  watermark.last_seen_ns = publisher.view.last_seen_ns;
  while (state.watermarks.size() > Limits::kMaxPublisherWatermarks) {
    state.watermarks.erase(state.watermarks.begin());
  }

  StaleEvidenceRecord record;
  record.kind = StaleEvidenceRecord::Kind::FencedPublisher;
  record.publisher = publisher_id;
  record.publisher_boot = boot;
  record.observed_at_ns = monotonic_now_ns();
  record.detail = std::string("publisher fenced (") + std::string(to_string(reason)) + "): " +
                  publisher.view.fence_detail;
  detail::record_stale(state, std::move(record));
  return Result<PublisherView>(publisher.view);
}

Result<PublisherView> Observatory::fence_publisher(const PublisherId& publisher_id,
                                                    FenceReason reason,
                                                    std::string detail_text) {
  PublisherBootId boot;
  {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->state.publishers.find(publisher_id.str());
    if (it == impl_->state.publishers.end()) {
      return fail_as<PublisherView>(ErrorCode::NotFound, "unknown publisher", publisher_id.str());
    }
    boot = it->second.view.boot;
  }
  // The lock is released before delegating; the fence path takes it again.
  return fence_publisher_boot(publisher_id, boot, reason, std::move(detail_text));
}

Status Observatory::fence_all_publishers(FenceReason reason, std::string detail_text) {
  std::vector<std::pair<PublisherId, PublisherBootId>> boots;
  {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& entry : impl_->state.publishers) {
      boots.emplace_back(entry.second.view.id, entry.second.view.boot);
    }
  }
  for (const auto& entry : boots) {
    const Result<PublisherView> fenced =
        fence_publisher_boot(entry.first, entry.second, reason, detail_text);
    if (!fenced.ok() && fenced.code() != ErrorCode::NotFound) {
      return Status(fenced.error());
    }
  }
  return Status();
}

Result<PublisherView> Observatory::publisher(const PublisherId& publisher_id) const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const auto it = impl_->state.publishers.find(publisher_id.str());
  if (it == impl_->state.publishers.end()) {
    return fail_as<PublisherView>(ErrorCode::NotFound, "unknown publisher", publisher_id.str());
  }
  return Result<PublisherView>(it->second.view);
}

// ---- Ingestion ---------------------------------------------------------

namespace {

using namespace sol::coherence::detail;

struct IngestContext {
  IngestionOutcome outcome;
};

/// Shared ingestion body.  Runs with the state mutex held.
IngestContext ingest_locked(State& state, Observation& observation) {
  IngestContext context;

  const Status structural = validate_observation_structure(observation);
  if (!structural.ok()) {
    context.outcome.disposition = IngestionDisposition::Rejected;
    context.outcome.code = structural.code();
    context.outcome.detail = structural.describe();
    state.loss.rejected_malformed += 1;
    return context;
  }

  const auto publisher_it = state.publishers.find(observation.source_publisher.str());
  if (publisher_it == state.publishers.end()) {
    context.outcome.disposition = IngestionDisposition::Rejected;
    context.outcome.code = ErrorCode::Unauthorized;
    context.outcome.detail = "unknown publisher";
    state.loss.rejected_unauthorized += 1;
    return context;
  }
  PublisherState& publisher = publisher_it->second;

  if (publisher.view.boot != observation.publisher_boot) {
    context.outcome.disposition = IngestionDisposition::Rejected;
    context.outcome.code = ErrorCode::StaleBoot;
    context.outcome.detail = "superseded publisher boot";
    state.loss.rejected_unauthorized += 1;
    return context;
  }
  if (observation.coordinator_epoch != state.coordinator_epoch) {
    context.outcome.disposition = IngestionDisposition::Rejected;
    context.outcome.code = ErrorCode::StaleEpoch;
    context.outcome.detail = "stale coordinator epoch";
    state.loss.rejected_unauthorized += 1;
    return context;
  }

  const bool publisher_current = publisher.view.current && !publisher.view.fenced;
  if (!publisher_current && state.options.enforce_current_publisher) {
    context.outcome.disposition = IngestionDisposition::Rejected;
    context.outcome.code = ErrorCode::StaleBoot;
    context.outcome.detail = "publisher boot is not current";
    state.loss.rejected_unauthorized += 1;
    return context;
  }

  const std::uint64_t content_hash = observation.content_hash();
  const SequenceDecision decision =
      account_sequence(publisher, observation.sequence.value(), content_hash);

  // A duplicate event id with different content is always a conflict.
  const auto prior_event =
      publisher.recent_event_ids.find(observation.event_id.value());
  if (prior_event != publisher.recent_event_ids.end() &&
      prior_event->second != content_hash && decision.mutate) {
    context.outcome.disposition = IngestionDisposition::Rejected;
    context.outcome.code = ErrorCode::Conflict;
    context.outcome.detail = "conflicting duplicate: same event id, different content";
    publisher.view.sequences.conflicting_duplicates += 1;
    publisher.view.rejected_events += 1;
    state.loss.rejected_conflicting_duplicates += 1;
    return context;
  }

  switch (decision.disposition) {
    case IngestionDisposition::Duplicate:
      publisher.view.sequences.duplicates += 1;
      state.loss.rejected_duplicates += 1;
      context.outcome.disposition = IngestionDisposition::Duplicate;
      context.outcome.code = ErrorCode::Duplicate;
      context.outcome.detail = decision.detail;
      return context;
    case IngestionDisposition::Rejected:
      if (decision.code == ErrorCode::StaleSequence) {
        publisher.view.sequences.rejected_stale += 1;
        state.loss.rejected_stale_sequences += 1;
      } else {
        publisher.view.sequences.conflicting_duplicates += 1;
        state.loss.rejected_conflicting_duplicates += 1;
      }
      publisher.view.rejected_events += 1;
      context.outcome.disposition = IngestionDisposition::Rejected;
      context.outcome.code = decision.code;
      context.outcome.detail = decision.detail;
      return context;
    default:
      break;
  }

  commit_sequence(publisher, observation.sequence.value(), content_hash);
  commit_event_id(publisher, observation.event_id.value(), content_hash);
  if (decision.disposition == IngestionDisposition::AcceptedLate) {
    publisher.view.sequences.late_events += 1;
    observation.late = true;
  }
  if (decision.missing != 0) {
    publisher.view.sequences.missing_events += decision.missing;
    publisher.view.sequences.sequence_gaps += 1;
    state.loss.missing_sequences += decision.missing;
  }
  publisher.view.sequences.accepted += 1;
  publisher.view.sequences.high_watermark = EventSequence{publisher.high_watermark};
  publisher.view.sequences.next_expected = EventSequence{publisher.high_watermark + 1};
  publisher.view.accepted_events += 1;
  publisher.view.last_seen_ns = monotonic_now_ns();

  observation.accepted_epoch = state.observation_epoch;

  const AttributionResult attribution =
      attribute_observation(state, observation);
  context.outcome.attribution = attribution.outcome;
  if (!attribution.reasons.empty()) {
    context.outcome.attribution_reason = attribution.reasons.front();
  }
  context.outcome.disposition = decision.disposition;
  context.outcome.code = ErrorCode::Ok;
  context.outcome.detail = decision.detail;
  context.outcome.missing_sequences = decision.missing;
  context.outcome.counted =
      attribution.outcome != AttributionOutcome::StaleEvidence &&
      attribution.outcome != AttributionOutcome::Unsupported;

  if (attribution.outcome == AttributionOutcome::StaleEvidence ||
      attribution.outcome == AttributionOutcome::Unsupported) {
    StaleEvidenceRecord record;
    record.event_id = observation.event_id;
    record.publisher = observation.source_publisher;
    record.publisher_boot = observation.publisher_boot;
    record.event_type = observation.type;
    record.observed_at_ns = observation.timestamp_ns;
    if (observation.region.has_value()) {
      record.region = *observation.region;
    }
    record.detail = "attribution outcome " + std::string(to_string(attribution.outcome)) +
                    " (" + (attribution.reasons.empty() ? std::string("no reason")
                                                        : attribution.reasons.front()) + ")";
    if (attribution.outcome == AttributionOutcome::StaleEvidence) {
      if (attribution.has_reason(attribution_reason::kRetiredRegion)) {
        record.kind = StaleEvidenceRecord::Kind::RetiredRegion;
      } else if (attribution.has_reason(attribution_reason::kStaleTopology)) {
        record.kind = StaleEvidenceRecord::Kind::TopologySuperseded;
      } else if (attribution.has_reason(attribution_reason::kStaleRegionGeneration)) {
        record.kind = StaleEvidenceRecord::Kind::StaleGeneration;
      } else {
        record.kind = StaleEvidenceRecord::Kind::StaleBoot;
      }
    } else {
      record.kind = StaleEvidenceRecord::Kind::StaleGeneration;
      record.detail = "unsupported observability claim: " + record.detail;
    }
    record_stale(state, std::move(record));
  }

  apply_observation(state, observation, attribution.outcome, attribution.locality,
                    attribution.locality_established);
  return context;
}

}  // namespace

Result<IngestionOutcome> Observatory::ingest(Observation observation) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  IngestContext context = ingest_locked(impl_->state, observation);
  return Result<IngestionOutcome>(std::move(context.outcome));
}

Result<BatchOutcome> Observatory::ingest_batch(std::vector<Observation> observations) {
  if (observations.empty()) {
    return fail_as<BatchOutcome>(ErrorCode::InvalidArgument, "empty batch");
  }
  if (observations.size() > Limits::kMaxBatchEvents) {
    return fail_as<BatchOutcome>(ErrorCode::TooLarge, "batch exceeds the maximum event count",
                                 detail::format_u64(observations.size()));
  }
  {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    State& state = impl_->state;
    const std::size_t limit = state.options.max_pending_ingestion;
    if (state.pending_ingestion + observations.size() > limit) {
      state.loss.backpressure_rejections += observations.size();
      return fail_as<BatchOutcome>(ErrorCode::Capacity,
                                   "ingestion capacity reached; batch rejected",
                                   detail::format_u64(state.pending_ingestion));
    }
    state.pending_ingestion += observations.size();
  }

  BatchOutcome outcome;
  outcome.per_event.reserve(observations.size());
  for (Observation& observation : observations) {
    const Result<IngestionOutcome> result = ingest(std::move(observation));
    if (!result.ok()) {
      IngestionOutcome rejected;
      rejected.disposition = IngestionDisposition::Rejected;
      rejected.code = result.code();
      rejected.detail = result.error().describe();
      outcome.per_event.push_back(std::move(rejected));
      outcome.rejected += 1;
      continue;
    }
    const IngestionOutcome& per_event = result.value();
    switch (per_event.disposition) {
      case IngestionDisposition::Accepted: outcome.accepted += 1; break;
      case IngestionDisposition::AcceptedLate: outcome.accepted_late += 1; break;
      case IngestionDisposition::Duplicate: outcome.duplicates += 1; break;
      case IngestionDisposition::Rejected: outcome.rejected += 1; break;
    }
    outcome.missing_sequences += per_event.missing_sequences;
    outcome.per_event.push_back(per_event);
  }

  {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->state.pending_ingestion -= observations.size();
  }
  return Result<BatchOutcome>(std::move(outcome));
}

Result<CounterOutcome> Observatory::ingest_counter(CounterPublication publication) {
  const Status structural = validate_counter_publication(publication);
  if (!structural.ok()) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->state.loss.rejected_malformed += 1;
    return Result<CounterOutcome>(structural.error());
  }
  if (publication.granularity == EvidenceGranularity::Unknown) {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->state.loss.rejected_malformed += 1;
    return fail_as<CounterOutcome>(ErrorCode::InvalidArgument,
                                   "a counter publication must state its evidence granularity");
  }

  const std::lock_guard<std::mutex> lock(impl_->mutex);
  State& state = impl_->state;

  const auto publisher_it = state.publishers.find(publication.publisher.str());
  if (publisher_it == state.publishers.end()) {
    state.loss.rejected_unauthorized += 1;
    return fail_as<CounterOutcome>(ErrorCode::Unauthorized, "unknown publisher");
  }
  PublisherState& publisher = publisher_it->second;
  if (publisher.view.boot != publication.publisher_boot) {
    state.loss.rejected_unauthorized += 1;
    return fail_as<CounterOutcome>(ErrorCode::StaleBoot, "superseded publisher boot");
  }
  if (publication.coordinator_epoch != state.coordinator_epoch) {
    state.loss.rejected_unauthorized += 1;
    return fail_as<CounterOutcome>(ErrorCode::StaleEpoch, "stale coordinator epoch");
  }
  if (!publisher.view.current || publisher.view.fenced) {
    state.loss.rejected_unauthorized += 1;
    return fail_as<CounterOutcome>(ErrorCode::StaleBoot, "publisher boot is not current");
  }

  // Counters consume the same per-publisher sequence space as events.
  const std::uint64_t counter_hash =
      detail::fnv1a_str(detail::fnv1a_str(detail::fnv1a_init(), publication.counter.view()),
                        detail::format_u64(publication.raw_value));
  const SequenceDecision decision =
      account_sequence(publisher, publication.sequence.value(), counter_hash);
  if (decision.disposition == IngestionDisposition::Duplicate) {
    publisher.view.sequences.duplicates += 1;
    state.loss.rejected_duplicates += 1;
    CounterOutcome outcome;
    outcome.detail = "duplicate counter sample";
    return Result<CounterOutcome>(std::move(outcome));
  }
  if (decision.disposition == IngestionDisposition::Rejected) {
    publisher.view.rejected_events += 1;
    if (decision.code == ErrorCode::StaleSequence) {
      state.loss.rejected_stale_sequences += 1;
    } else {
      state.loss.rejected_conflicting_duplicates += 1;
    }
    return fail_as<CounterOutcome>(decision.code, decision.detail);
  }
  commit_sequence(publisher, publication.sequence.value(), counter_hash);
  if (decision.disposition == IngestionDisposition::AcceptedLate) {
    publisher.view.sequences.late_events += 1;
  }
  if (decision.missing != 0) {
    publisher.view.sequences.missing_events += decision.missing;
    publisher.view.sequences.sequence_gaps += 1;
    state.loss.missing_sequences += decision.missing;
  }
  publisher.view.sequences.accepted += 1;
  publisher.view.sequences.high_watermark = EventSequence{publisher.high_watermark};
  publisher.view.sequences.next_expected = EventSequence{publisher.high_watermark + 1};
  publisher.view.accepted_counters += 1;
  publisher.view.last_seen_ns = monotonic_now_ns();

  CounterKey key;
  key.publisher = publication.publisher.str();
  key.boot = publication.publisher_boot.value();
  key.counter = publication.counter.str();

  if (state.counters.find(key) == state.counters.end() &&
      state.counters.size() >= Limits::kMaxCounterRecords) {
    state.loss.rejected_overflow += 1;
    return fail_as<CounterOutcome>(ErrorCode::Capacity, "counter capacity reached");
  }
  CounterState& counter = state.counters[key];

  CounterOutcome outcome;
  const bool identity_changed =
      counter.samples != 0 && (counter.generation != publication.generation ||
                               counter.kind != publication.kind ||
                               counter.scope != publication.scope ||
                               counter.width_bits != publication.width_bits ||
                               counter.mapped_type != publication.mapped_type);
  if (identity_changed) {
    outcome.discontinuity = true;
    outcome.discontinuity_reason = "counter generation or descriptor changed";
    counter.accumulated = 0;
    counter.has_last = false;
    counter.saturated = false;
    publisher.view.sequences.counter_resets += 1;
  }

  counter.kind = publication.kind;
  counter.scope = publication.scope;
  counter.width_bits = publication.width_bits;
  counter.generation = publication.generation;
  counter.bytes_per_unit = publication.bytes_per_unit;
  counter.mapped_type = publication.mapped_type;
  counter.provenance = publication.provenance;
  counter.granularity = publication.granularity;
  counter.source = publication.source;
  counter.target = publication.target;
  counter.region = publication.region;
  counter.region_generation = publication.region_generation;
  counter.workload = publication.workload;
  counter.topology_generation = publication.topology_generation;

  const std::uint64_t width_max =
      publication.width_bits >= 64 ? std::numeric_limits<std::uint64_t>::max()
                                   : ((1ull << publication.width_bits) - 1ull);
  if (publication.kind != CounterKind::Delta && publication.kind != CounterKind::Sampled &&
      publication.raw_value > width_max) {
    state.loss.rejected_malformed += 1;
    return fail_as<CounterOutcome>(ErrorCode::OutOfRange,
                                   "counter value exceeds its declared width");
  }

  std::uint64_t delta = 0;
  switch (publication.kind) {
    case CounterKind::Delta:
      delta = publication.raw_value;
      break;
    case CounterKind::Sampled:
      delta = 0;
      break;
    default:
      if (!counter.has_last) {
        // The first sample establishes a baseline; prior traffic cannot be
        // attributed to this interval and is not invented.
        delta = 0;
      } else if (publication.raw_value >= counter.last_raw) {
        delta = publication.raw_value - counter.last_raw;
      } else if (publication.kind == CounterKind::Wrapping && publication.width_bits < 64 &&
                 (counter.last_raw - publication.raw_value) > (width_max / 2)) {
        delta = (width_max - counter.last_raw) + publication.raw_value + 1;
        counter.wraps += 1;
        publisher.view.sequences.counter_wraps += 1;
        outcome.discontinuity = true;
        outcome.discontinuity_reason = "counter wrapped";
      } else {
        // A reset is never negative traffic and never fabricated traffic.
        delta = 0;
        counter.resets += 1;
        publisher.view.sequences.counter_resets += 1;
        outcome.discontinuity = true;
        outcome.discontinuity_reason = "counter reset";
      }
      break;
  }

  counter.has_last = true;
  counter.last_raw = publication.raw_value;
  counter.samples += 1;
  if (!counter.has_time) {
    counter.first_ns = publication.timestamp_ns;
    counter.has_time = true;
  }
  counter.last_ns = publication.timestamp_ns;
  const AddResult accumulated = checked_add(counter.accumulated, delta);
  counter.accumulated = accumulated.value;
  if (accumulated.overflowed) {
    counter.saturated = true;
  }

  outcome.accepted = true;
  outcome.delta = delta;
  outcome.produced_delta = delta != 0;
  if (!outcome.discontinuity) {
    outcome.discontinuity_reason.clear();
  }

  if (delta == 0) {
    return Result<CounterOutcome>(std::move(outcome));
  }

  // Turn the delta into a first-class observation so that aggregation,
  // attribution and analysis treat counter evidence exactly like any other.
  Observation derived;
  derived.event_id = counter_event_id(key.counter, publication.sequence);
  derived.type = publication.mapped_type;
  derived.source_publisher = publication.publisher;
  derived.publisher_boot = publication.publisher_boot;
  derived.coordinator_epoch = state.coordinator_epoch;
  derived.sampling_epoch = publication.sampling_epoch;
  derived.evidence_generation = publication.evidence_generation;
  derived.sequence = publication.sequence;
  derived.timestamp_ns = publication.timestamp_ns;
  derived.source = publication.source;
  derived.target = publication.target;
  derived.region = publication.region;
  derived.region_generation = publication.region_generation;
  derived.topology_generation = publication.topology_generation;
  derived.workload = publication.workload;
  switch (publication.mapped_type) {
    case EventType::RemoteRead:
    case EventType::SharedRead:
    case EventType::DirectoryLookup:
      derived.direction = AccessDirection::Read;
      break;
    case EventType::RemoteWrite:
    case EventType::Writeback:
    case EventType::WriteExclusiveTransition:
      derived.direction = AccessDirection::Write;
      break;
    case EventType::RemoteAtomic:
      derived.direction = AccessDirection::Atomic;
      break;
    default:
      derived.direction = AccessDirection::None;
      break;
  }
  derived.counter_delta = delta;
  derived.counter_generation = publication.generation;
  const AddResult scaled = checked_mul(delta, publication.bytes_per_unit);
  derived.bytes = scaled.value > Limits::kMaxByteCount ? Limits::kMaxByteCount : scaled.value;
  derived.precision = Precision::ExactCounterDelta;
  derived.granularity = publication.granularity;
  derived.provenance = publication.provenance;
  derived.locality = Locality::Unknown;
  derived.locality_declared = false;
  derived.metadata = publication.metadata;
  derived.accepted_epoch = state.observation_epoch;

  const AttributionResult attribution = attribute_observation(state, derived);
  if (attribution.outcome == AttributionOutcome::StaleEvidence ||
      attribution.outcome == AttributionOutcome::Unsupported) {
    StaleEvidenceRecord record;
    record.kind = StaleEvidenceRecord::Kind::StaleGeneration;
    record.event_id = derived.event_id;
    record.publisher = derived.source_publisher;
    record.publisher_boot = derived.publisher_boot;
    record.event_type = derived.type;
    record.observed_at_ns = derived.timestamp_ns;
    if (derived.region.has_value()) {
      record.region = *derived.region;
    }
    record.detail = "counter-derived evidence not current: " +
                    (attribution.reasons.empty() ? std::string("no reason")
                                                 : attribution.reasons.front());
    record_stale(state, std::move(record));
  }
  apply_observation(state, derived, attribution.outcome, attribution.locality,
                    attribution.locality_established);
  return Result<CounterOutcome>(std::move(outcome));
}

// ---- Attribution -------------------------------------------------------

Result<AttributionResult> Observatory::attribute(const Observation& observation) const {
  const Status structural = validate_observation_structure(observation);
  if (!structural.ok()) {
    return Result<AttributionResult>(structural.error());
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return Result<AttributionResult>(detail::attribute_observation(impl_->state, observation));
}

Result<AttributionResult> Observatory::attribute_region(const MemoryRegionId& region) const {
  const Status token = validate_identity_token(region.view());
  if (!token.ok()) {
    return Result<AttributionResult>(token.error());
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return Result<AttributionResult>(detail::attribute_region_state(impl_->state, region));
}

// ---- Queries -----------------------------------------------------------

SnapshotPtr Observatory::snapshot(const FindingsOptions& options) const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->state.snapshot_generation = impl_->state.snapshot_generation.next();
  return detail::SnapshotAssembler::build(impl_->state, options);
}

Result<std::vector<Finding>> Observatory::findings(const FindingsOptions& options) const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return Result<std::vector<Finding>>(detail::analyze_all(impl_->state, options));
}

Result<RegionAnalysis> Observatory::analyze_region(const MemoryRegionId& region,
                                                    const FindingsOptions& options) const {
  const Status token = validate_identity_token(region.view());
  if (!token.ok()) {
    return Result<RegionAnalysis>(token.error());
  }
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const State& state = impl_->state;

  RegionAnalysis analysis;
  analysis.region = region;
  const auto it = state.regions.find(region.str());
  if (it != state.regions.end()) {
    analysis.registered = true;
    analysis.retired = it->second.retired;
    analysis.owner = it->second.owner;
    analysis.sharing_scope = it->second.sharing_scope;
    analysis.memory_domain = it->second.memory_domain;
    analysis.generation = it->second.generation;
  } else {
    analysis.missing_evidence.push_back("region.registration");
  }
  if (const AggregateValue* bucket =
          state.aggregates.find(AggregateKey{AggregateDimension::Region, region.str()});
      bucket != nullptr) {
    analysis.observations = bucket->observations;
    analysis.bytes = bucket->bytes;
    analysis.remote_accesses = bucket->remote_accesses;
    analysis.invalidations = bucket->invalidations;
    analysis.ownership_transfers = bucket->ownership_transfers;
    analysis.precision = bucket->weakest_precision;
    analysis.reality = bucket->reality();
  } else {
    analysis.missing_evidence.push_back("region aggregation bucket");
  }
  analysis.provenance = Provenance::DerivedAggregation;
  analysis.bindings.coordinator_epoch = state.coordinator_epoch;
  analysis.bindings.observation_epoch = state.observation_epoch;
  analysis.bindings.topology_generation = state.topology_generation;
  analysis.bindings.region_generation = analysis.generation;
  analysis.bindings.region_generation_bound = analysis.registered;

  const auto evidence_it = state.region_evidence.find(region.str());
  if (evidence_it != state.region_evidence.end()) {
    for (const detail::OwnershipSample& sample : evidence_it->second.samples) {
      if (analysis.evidence.size() >= Limits::kMaxEvidenceRefs) {
        break;
      }
      EvidenceRef ref;
      ref.event_id = sample.event_id;
      ref.publisher = sample.publisher;
      ref.publisher_boot = sample.publisher_boot;
      ref.sequence = sample.sequence;
      ref.event_type = sample.type;
      ref.precision = sample.precision;
      ref.provenance = sample.provenance;
      analysis.evidence.push_back(ref);
    }
    if (evidence_it->second.evicted != 0) {
      analysis.missing_evidence.push_back(
          "region sample window evicted " + detail::format_u64(evidence_it->second.evicted) +
          " older samples");
    }
  } else {
    analysis.missing_evidence.push_back("per-region pattern samples");
  }

  std::vector<Finding> all = detail::analyze_all(state, options);
  for (Finding& finding : all) {
    if (finding.subject_kind == "region" && finding.subject == region.str()) {
      if (analysis.findings.size() < Limits::kMaxFindings) {
        analysis.findings.push_back(std::move(finding));
      }
    }
  }
  detail::CostInputs inputs;
  inputs.bytes = analysis.bytes;
  inputs.remote_reads = 0;
  inputs.remote_writes = 0;
  inputs.invalidations = analysis.invalidations;
  inputs.ownership_transfers = analysis.ownership_transfers;
  inputs.precision = analysis.precision;
  analysis.cost = detail::estimate_cost(state.options.cost_model, inputs);
  return Result<RegionAnalysis>(std::move(analysis));
}

Result<std::vector<RemoteAccessRow>> Observatory::remote_access(
    const RemoteAccessPolicy& policy) const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<RemoteAccessRow> rows;
  detail::analyze_remote_access(impl_->state, policy, &rows);
  while (rows.size() > policy.max_rows) {
    rows.pop_back();
  }
  return Result<std::vector<RemoteAccessRow>>(std::move(rows));
}

Result<std::vector<InvalidationRow>> Observatory::invalidation_analysis(
    const InvalidationPolicy& policy) const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<InvalidationRow> rows;
  detail::analyze_invalidations(impl_->state, policy, &rows);
  while (rows.size() > Limits::kMaxQueryResults) {
    rows.pop_back();
  }
  return Result<std::vector<InvalidationRow>>(std::move(rows));
}

Result<std::vector<Finding>> Observatory::ping_pong(const PingPongPolicy& policy) const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return Result<std::vector<Finding>>(detail::analyze_ping_pong(impl_->state, policy));
}

Result<std::vector<Finding>> Observatory::hotspots(const HotspotPolicy& policy) const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return Result<std::vector<Finding>>(detail::analyze_hotspots(impl_->state, policy));
}

Result<std::vector<Finding>> Observatory::false_sharing(const FalseSharingPolicy& policy) const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return Result<std::vector<Finding>>(detail::analyze_false_sharing(impl_->state, policy));
}

// ---- Reconciliation ----------------------------------------------------

Result<ReconcileReport> Observatory::reconcile_current_evidence() {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  State& state = impl_->state;
  ReconcileReport report;
  state.observation_epoch = state.observation_epoch.next();
  report.coordinator_epoch = state.coordinator_epoch;
  report.observation_epoch = state.observation_epoch;
  for (auto& entry : state.publishers) {
    if (!entry.second.view.current) {
      report.publishers_marked_stale += 1;
    }
    entry.second.view.evidence_generation = entry.second.view.evidence_generation.next();
    report.evidence_generations_retired += 1;
    StaleEvidenceRecord record;
    record.kind = StaleEvidenceRecord::Kind::StaleGeneration;
    record.publisher = entry.second.view.id;
    record.publisher_boot = entry.second.view.boot;
    record.observed_at_ns = monotonic_now_ns();
    record.detail = "evidence generation retired by reconciliation; fresh publication required";
    detail::record_stale(state, std::move(record));
    report.stale_records += 1;
  }
  report.findings_invalidated = state.stale.size();
  return Result<ReconcileReport>(std::move(report));
}

// ---- Persistence -------------------------------------------------------

Result<PersistenceReport> Observatory::save_state(const std::filesystem::path& path,
                                                   const PersistenceOptions& options) {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  return detail::save_state(impl_->state, path, options);
}

Result<PersistenceReport> Observatory::load_state(const std::filesystem::path& path,
                                                   const PersistenceOptions& options) {
  std::unique_lock<std::mutex> lock(impl_->mutex);
  return detail::load_state(impl_->state, path, options);
}

// ---- Diagnostics -------------------------------------------------------

std::uint64_t Observatory::state_fingerprint() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return detail::state_fingerprint(impl_->state);
}

LossReport Observatory::loss_report() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->state.loss;
}

std::vector<Capability> Observatory::capabilities() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  std::vector<Capability> result;
  result.reserve(impl_->state.capabilities.size());
  for (const auto& entry : impl_->state.capabilities) {
    result.push_back(entry.second);
  }
  sort_capabilities(result);
  return result;
}

}  // namespace sol::coherence
