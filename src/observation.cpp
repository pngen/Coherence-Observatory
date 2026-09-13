// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/observation.hpp"

#include <algorithm>

#include "text_util.hpp"

namespace sol::coherence {
namespace {

constexpr std::string_view kBackendEventKey = "backend.event";

Status validate_resource_ref(const ResourceRef& ref, const char* role) {
  if (ref.id.empty()) {
    return fail(ErrorCode::InvalidArgument, std::string(role) + " resource id is empty");
  }
  const Status token = validate_identity_token(ref.id.view());
  if (!token.ok()) {
    return Status(Error(token.code(), std::string(role) + " resource id invalid",
                        token.error().message()));
  }
  switch (ref.kind) {
    case ResourceKind::Node:
      if (ref.generation != 0) {
        return fail(ErrorCode::InvalidArgument,
                    std::string(role) + " node reference must not carry a device generation");
      }
      break;
    case ResourceKind::Processor:
    case ResourceKind::Accelerator:
    case ResourceKind::MemoryDomain:
    case ResourceKind::CoherenceDomain:
      if (ref.generation == 0) {
        return fail(ErrorCode::InvalidArgument,
                    std::string(role) + " reference requires a non-zero generation");
      }
      break;
    case ResourceKind::Unknown:
      return fail(ErrorCode::InvalidArgument,
                  std::string(role) + " reference has unknown resource kind");
  }
  return Status();
}

}  // namespace

Status BoundedMetadata::add(std::string_view key, std::string_view value) {
  if (key.empty()) {
    return fail(ErrorCode::InvalidArgument, "metadata key must not be empty");
  }
  if (key.size() > Limits::kMaxMetadataKeyLength) {
    return fail(ErrorCode::TooLarge, "metadata key exceeds maximum length");
  }
  if (value.size() > Limits::kMaxMetadataValueLength) {
    return fail(ErrorCode::TooLarge, "metadata value exceeds maximum length");
  }
  if (entries_.size() >= Limits::kMaxMetadataEntries) {
    return fail(ErrorCode::TooMany, "metadata entry limit reached");
  }
  for (const auto& entry : entries_) {
    if (entry.first == key) {
      return fail(ErrorCode::AlreadyExists, "duplicate metadata key", std::string(key));
    }
  }
  entries_.emplace_back(std::string(key), std::string(value));
  return Status();
}

const std::string* BoundedMetadata::find(std::string_view key) const noexcept {
  for (const auto& entry : entries_) {
    if (entry.first == key) {
      return &entry.second;
    }
  }
  return nullptr;
}

std::vector<std::pair<std::string, std::string>> BoundedMetadata::sorted_entries() const {
  std::vector<std::pair<std::string, std::string>> out = entries_;
  std::sort(out.begin(), out.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  return out;
}

std::uint64_t BoundedMetadata::canonical_hash() const noexcept {
  std::uint64_t h = detail::fnv1a_init();
  for (const auto& entry : sorted_entries()) {
    h = detail::fnv1a_str(h, entry.first);
    h = detail::fnv1a_str(h, entry.second);
  }
  return h;
}

std::uint64_t Observation::content_hash() const noexcept {
  std::uint64_t h = detail::fnv1a_init();
  h = detail::fnv1a_u64(h, event_id.value());
  h = detail::fnv1a_u64(h, static_cast<std::uint64_t>(type));
  h = detail::fnv1a_str(h, source_publisher.view());
  h = detail::fnv1a_u64(h, publisher_boot.value());
  h = detail::fnv1a_u64(h, coordinator_epoch.value());
  h = detail::fnv1a_u64(h, sampling_epoch.value());
  h = detail::fnv1a_u64(h, evidence_generation.value());
  h = detail::fnv1a_u64(h, sequence.value());
  h = detail::fnv1a_u64(h, static_cast<std::uint64_t>(timestamp_ns));
  if (source.has_value()) {
    h = detail::fnv1a_u64(h, 1);
    h = detail::fnv1a_u64(h, static_cast<std::uint64_t>(source->kind));
    h = detail::fnv1a_str(h, source->id.view());
    h = detail::fnv1a_u64(h, source->generation);
  }
  if (target.has_value()) {
    h = detail::fnv1a_u64(h, 2);
    h = detail::fnv1a_u64(h, static_cast<std::uint64_t>(target->kind));
    h = detail::fnv1a_str(h, target->id.view());
    h = detail::fnv1a_u64(h, target->generation);
  }
  if (region.has_value()) {
    h = detail::fnv1a_u64(h, 3);
    h = detail::fnv1a_str(h, region->view());
    h = detail::fnv1a_u64(h, region_generation.has_value() ? region_generation->value() : 0);
  }
  if (coherence_domain.has_value()) {
    h = detail::fnv1a_u64(h, 4);
    h = detail::fnv1a_str(h, coherence_domain->view());
    h = detail::fnv1a_u64(
        h, coherence_domain_generation.has_value() ? coherence_domain_generation->value() : 0);
  }
  h = detail::fnv1a_u64(h, topology_generation.value());
  if (workload.has_value()) {
    h = detail::fnv1a_str(h, workload->view());
  }
  if (process.has_value()) {
    h = detail::fnv1a_str(h, process->view());
  }
  h = detail::fnv1a_u64(h, static_cast<std::uint64_t>(direction));
  h = detail::fnv1a_u64(h, static_cast<std::uint64_t>(state_before));
  h = detail::fnv1a_u64(h, static_cast<std::uint64_t>(state_after));
  if (counter_delta.has_value()) {
    h = detail::fnv1a_u64(h, 5);
    h = detail::fnv1a_u64(h, *counter_delta);
  }
  h = detail::fnv1a_u64(h, counter_generation.value());
  h = detail::fnv1a_u64(h, bytes);
  h = detail::fnv1a_u64(h, lines);
  h = detail::fnv1a_u64(h, pages);
  h = detail::fnv1a_u64(h, region_count);
  h = detail::fnv1a_u64(h, static_cast<std::uint64_t>(measured_duration_ns));
  h = detail::fnv1a_u64(h, static_cast<std::uint64_t>(locality));
  h = detail::fnv1a_u64(h, locality_declared ? 1u : 0u);
  h = detail::fnv1a_u64(h, static_cast<std::uint64_t>(provenance));
  h = detail::fnv1a_u64(h, static_cast<std::uint64_t>(precision));
  h = detail::fnv1a_u64(h, static_cast<std::uint64_t>(granularity));
  h = detail::fnv1a_u64(h, metadata.canonical_hash());
  return h;
}

Status validate_observation_structure(const Observation& observation) {
  if (observation.event_id.is_zero()) {
    return fail(ErrorCode::InvalidArgument, "event id must be non-zero");
  }
  if (static_cast<std::size_t>(observation.type) >= kEventTypeCount) {
    return fail(ErrorCode::InvalidArgument, "event type out of range");
  }
  if (observation.source_publisher.empty()) {
    return fail(ErrorCode::InvalidArgument, "source publisher is empty");
  }
  const Status publisher_token = validate_identity_token(observation.source_publisher.view());
  if (!publisher_token.ok()) {
    return Status(Error(publisher_token.code(), "source publisher identity invalid",
                        publisher_token.error().message()));
  }
  if (observation.publisher_boot.is_zero()) {
    return fail(ErrorCode::InvalidArgument, "publisher boot identity must be non-zero");
  }
  if (observation.timestamp_ns < 0 || observation.timestamp_ns > Limits::kMaxTimestampNs) {
    return fail(ErrorCode::OutOfRange, "observation timestamp out of range");
  }
  if (observation.sequence.is_zero()) {
    return fail(ErrorCode::InvalidArgument, "event sequence must be non-zero");
  }

  if (observation.source.has_value()) {
    const Status s = validate_resource_ref(*observation.source, "source");
    if (!s.ok()) return s;
  }
  if (observation.target.has_value()) {
    const Status s = validate_resource_ref(*observation.target, "target");
    if (!s.ok()) return s;
  }
  if (observation.source.has_value() && observation.target.has_value() &&
      observation.source->kind == observation.target->kind &&
      observation.source->id == observation.target->id) {
    return fail(ErrorCode::InvalidArgument,
                "source and target refer to the same resource",
                std::string(observation.source->id.view()));
  }

  if (observation.region.has_value()) {
    const Status s = validate_identity_token(observation.region->view());
    if (!s.ok()) {
      return Status(Error(s.code(), "region identity invalid", s.error().message()));
    }
    if (!observation.region_generation.has_value() ||
        observation.region_generation->is_zero()) {
      return fail(ErrorCode::InvalidArgument,
                  "a region reference must carry a non-zero region generation");
    }
  } else if (observation.region_generation.has_value()) {
    return fail(ErrorCode::InvalidArgument, "region generation present without a region");
  }

  if (observation.coherence_domain.has_value()) {
    const Status s = validate_identity_token(observation.coherence_domain->view());
    if (!s.ok()) {
      return Status(Error(s.code(), "coherence domain identity invalid", s.error().message()));
    }
    if (!observation.coherence_domain_generation.has_value() ||
        observation.coherence_domain_generation->is_zero()) {
      return fail(ErrorCode::InvalidArgument,
                  "a coherence domain reference must carry a non-zero generation");
    }
  } else if (observation.coherence_domain_generation.has_value()) {
    return fail(ErrorCode::InvalidArgument,
                "coherence domain generation present without a domain");
  }

  if (observation.workload.has_value()) {
    const Status s = validate_identity_token(observation.workload->view());
    if (!s.ok()) {
      return Status(Error(s.code(), "workload identity invalid", s.error().message()));
    }
  }
  if (observation.process.has_value()) {
    const Status s = validate_identity_token(observation.process->view());
    if (!s.ok()) {
      return Status(Error(s.code(), "process identity invalid", s.error().message()));
    }
  }

  if (static_cast<std::size_t>(observation.direction) > 5) {
    return fail(ErrorCode::InvalidArgument, "access direction out of range");
  }
  if (static_cast<std::size_t>(observation.state_before) > 9 ||
      static_cast<std::size_t>(observation.state_after) > 9) {
    return fail(ErrorCode::InvalidArgument, "coherence state out of range");
  }
  if (static_cast<std::size_t>(observation.locality) > kLocalityCount) {
    return fail(ErrorCode::InvalidArgument, "locality class out of range");
  }
  if (static_cast<std::size_t>(observation.provenance) > 9) {
    return fail(ErrorCode::InvalidArgument, "provenance out of range");
  }
  if (static_cast<std::size_t>(observation.precision) > 6) {
    return fail(ErrorCode::InvalidArgument, "precision out of range");
  }
  if (static_cast<std::size_t>(observation.granularity) > 6) {
    return fail(ErrorCode::InvalidArgument, "evidence granularity out of range");
  }

  if (observation.bytes > Limits::kMaxByteCount) {
    return fail(ErrorCode::OutOfRange, "byte count exceeds the maximum accepted value");
  }
  if (observation.lines > Limits::kMaxLineCount || observation.pages > Limits::kMaxLineCount) {
    return fail(ErrorCode::OutOfRange, "line/page count exceeds the maximum accepted value");
  }
  if (observation.region_count > Limits::kMaxRegionCount) {
    return fail(ErrorCode::OutOfRange, "region count exceeds the maximum accepted value");
  }
  if (observation.measured_duration_ns < -1 ||
      observation.measured_duration_ns > Limits::kMaxMeasuredDurationNs) {
    return fail(ErrorCode::OutOfRange, "measured duration out of range");
  }

  // An exact claim must state what it resolves.
  if (precision_rank(observation.precision) >= precision_rank(Precision::ExactCounterDelta) &&
      observation.granularity == EvidenceGranularity::Unknown) {
    return fail(ErrorCode::InvalidArgument,
                "exact precision requires an explicit evidence granularity");
  }

  // A counter delta is not an individually identified event.
  if (observation.counter_delta.has_value() &&
      observation.precision == Precision::ExactEvent) {
    return fail(ErrorCode::InvalidArgument,
                "counter-derived evidence must not claim exact-event precision");
  }
  if (observation.counter_delta.has_value() && observation.counter_generation.is_zero()) {
    return fail(ErrorCode::InvalidArgument,
                "counter-derived evidence requires a non-zero counter generation");
  }
  if (!observation.counter_delta.has_value() && !observation.counter_generation.is_zero()) {
    return fail(ErrorCode::InvalidArgument,
                "counter generation present without a counter delta");
  }

  // Locality is either declared with evidence or left unknown.
  if (!observation.locality_declared && observation.locality != Locality::Unknown) {
    return fail(ErrorCode::InvalidArgument,
                "locality class present without the declared flag");
  }

  // Unmappable backend events must retain their backend-native identity.
  if (observation.type == EventType::UnknownCoherenceEvent &&
      !observation.metadata.contains(kBackendEventKey)) {
    return fail(ErrorCode::InvalidArgument,
                "unknown coherence event must retain backend event detail",
                std::string(kBackendEventKey));
  }

  return Status();
}

Status validate_counter_publication(const CounterPublication& publication) {
  if (publication.publisher.empty()) {
    return fail(ErrorCode::InvalidArgument, "counter publisher is empty");
  }
  if (publication.publisher_boot.is_zero()) {
    return fail(ErrorCode::InvalidArgument, "counter publisher boot must be non-zero");
  }
  if (publication.counter.empty()) {
    return fail(ErrorCode::InvalidArgument, "counter identity is empty");
  }
  const Status token = validate_identity_token(publication.counter.view());
  if (!token.ok()) {
    return Status(Error(token.code(), "counter identity invalid", token.error().message()));
  }
  if (static_cast<std::size_t>(publication.mapped_type) >= kEventTypeCount) {
    return fail(ErrorCode::InvalidArgument, "counter mapped event type out of range");
  }
  if (static_cast<std::size_t>(publication.kind) > 4) {
    return fail(ErrorCode::InvalidArgument, "counter kind out of range");
  }
  if (static_cast<std::size_t>(publication.scope) > 6) {
    return fail(ErrorCode::InvalidArgument, "counter scope out of range");
  }
  if (publication.scope == CounterScope::Unknown) {
    return fail(ErrorCode::InvalidArgument, "counter scope must be stated");
  }
  if (publication.width_bits == 0 || publication.width_bits > Limits::kMaxCounterWidth) {
    return fail(ErrorCode::OutOfRange, "counter width out of range");
  }
  if (publication.bytes_per_unit > Limits::kMaxByteCount) {
    return fail(ErrorCode::OutOfRange, "counter bytes-per-unit out of range");
  }
  if (publication.sequence.is_zero()) {
    return fail(ErrorCode::InvalidArgument, "counter sequence must be non-zero");
  }
  if (publication.timestamp_ns < 0 || publication.timestamp_ns > Limits::kMaxTimestampNs) {
    return fail(ErrorCode::OutOfRange, "counter timestamp out of range");
  }
  if (static_cast<std::size_t>(publication.provenance) > 9) {
    return fail(ErrorCode::InvalidArgument, "counter provenance out of range");
  }
  if (static_cast<std::size_t>(publication.granularity) > 6) {
    return fail(ErrorCode::InvalidArgument, "counter granularity out of range");
  }

  if (publication.region.has_value()) {
    if (publication.scope != CounterScope::PerRegion) {
      return fail(ErrorCode::InvalidArgument,
                  "a counter that is not genuinely per-region must not name a region",
                  std::string(to_string(publication.scope)));
    }
    if (!publication.region_generation.has_value() ||
        publication.region_generation->is_zero()) {
      return fail(ErrorCode::InvalidArgument,
                  "per-region counter requires a non-zero region generation");
    }
  } else if (publication.scope == CounterScope::PerRegion) {
    return fail(ErrorCode::InvalidArgument,
                "per-region counter requires a region identity");
  } else if (publication.region_generation.has_value()) {
    return fail(ErrorCode::InvalidArgument,
                "region generation present without a region on a counter");
  }

  if (publication.scope != CounterScope::PerRegion) {
    // Only genuinely per-region counters may resolve below device granularity.
    if (granularity_rank(publication.granularity) > granularity_rank(EvidenceGranularity::Device)) {
      return fail(ErrorCode::InvalidArgument,
                  "counter scope cannot support the claimed evidence granularity",
                  std::string(to_string(publication.scope)));
    }
  }

  if (publication.scope == CounterScope::PerDevice || publication.scope == CounterScope::PerDomain ||
      publication.scope == CounterScope::PerLink) {
    if (!publication.source.has_value()) {
      return fail(ErrorCode::InvalidArgument,
                  "a device/domain/link scoped counter requires a source resource");
    }
  }

  return Status();
}

}  // namespace sol::coherence
