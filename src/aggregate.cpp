// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/aggregate.hpp"

#include <array>

#include "coherence/checked.hpp"
#include "text_util.hpp"

namespace sol::coherence {
namespace {

constexpr std::array<std::string_view, kAggregateDimensionCount> kDimensionNames = {
    "EVENT_TYPE", "DEVICE", "DOMAIN", "REGION", "WORKLOAD",
    "LOCALITY",   "TIME_BUCKET", "PUBLISHER", "COHERENCE_DOMAIN", "SOURCE_TARGET_PAIR"};

constexpr std::string_view kUnknownBucket = "unknown";

bool is_remote_access_type(EventType type) noexcept {
  switch (type) {
    case EventType::RemoteRead:
    case EventType::RemoteWrite:
    case EventType::RemoteAtomic:
      return true;
    default:
      return false;
  }
}

const ResourceRef* actor_of(const Observation& observation) noexcept {
  if (observation.source.has_value()) {
    return &*observation.source;
  }
  if (observation.target.has_value()) {
    return &*observation.target;
  }
  return nullptr;
}

}  // namespace

std::string_view to_string(AggregateDimension dimension) noexcept {
  const auto index = static_cast<std::size_t>(dimension);
  return index < kDimensionNames.size() ? kDimensionNames[index]
                                        : std::string_view("UNKNOWN");
}

Reality AggregateValue::reality() const noexcept {
  if (mixed_contribution) {
    return Reality::Mixed;
  }
  return synthetic_contribution ? Reality::Synthetic : Reality::Real;
}

double AggregateValue::rate_per_second() const noexcept {
  if (!has_timestamps || last_timestamp_ns <= first_timestamp_ns) {
    return 0.0;
  }
  const double span_ns = static_cast<double>(last_timestamp_ns - first_timestamp_ns);
  return static_cast<double>(observations) * 1.0e9 / span_ns;
}

std::string resource_aggregate_text(const ResourceRef& ref) {
  if (ref.empty()) {
    return std::string(kUnknownBucket);
  }
  return ref.id.str();
}

std::string aggregate_value_text(const Observation& observation, AggregateDimension dimension,
                                 std::int64_t time_bucket_ns) {
  switch (dimension) {
    case AggregateDimension::EventType:
      return std::string(to_string(observation.type));
    case AggregateDimension::Device: {
      const ResourceRef* actor = actor_of(observation);
      if (actor == nullptr) {
        return std::string(kUnknownBucket);
      }
      return actor->id.str();
    }
    case AggregateDimension::Domain: {
      if (observation.source.has_value() &&
          observation.source->kind == ResourceKind::MemoryDomain) {
        return observation.source->id.str();
      }
      if (observation.target.has_value() &&
          observation.target->kind == ResourceKind::MemoryDomain) {
        return observation.target->id.str();
      }
      return std::string(kUnknownBucket);
    }
    case AggregateDimension::Region:
      return observation.region.has_value() ? observation.region->str()
                                            : std::string(kUnknownBucket);
    case AggregateDimension::Workload:
      return observation.workload.has_value() ? observation.workload->str()
                                              : std::string(kUnknownBucket);
    case AggregateDimension::Locality:
      if (!observation.locality_declared || observation.locality == Locality::Unknown) {
        return std::string(kUnknownBucket);
      }
      return std::string(to_string(observation.locality));
    case AggregateDimension::TimeBucket: {
      if (time_bucket_ns <= 0 || observation.timestamp_ns < 0) {
        return std::string(kUnknownBucket);
      }
      const std::int64_t bucket = observation.timestamp_ns / time_bucket_ns;
      return detail::format_i64(bucket);
    }
    case AggregateDimension::Publisher:
      return observation.source_publisher.empty() ? std::string(kUnknownBucket)
                                                  : observation.source_publisher.str();
    case AggregateDimension::CoherenceDomain:
      return observation.coherence_domain.has_value() ? observation.coherence_domain->str()
                                                      : std::string(kUnknownBucket);
    case AggregateDimension::SourceTargetPair: {
      std::string value;
      value.append(observation.source.has_value() ? observation.source->id.view()
                                                  : kUnknownBucket);
      value.append("->");
      value.append(observation.target.has_value() ? observation.target->id.view()
                                                  : kUnknownBucket);
      return value;
    }
  }
  return std::string(kUnknownBucket);
}

ObservationContribution contribution_of(const Observation& observation) {
  ObservationContribution contribution;
  contribution.bytes = observation.bytes;
  contribution.lines = observation.lines;
  contribution.pages = observation.pages;
  contribution.regions_touched = observation.region_count;
  const bool remote_locality =
      observation.locality_declared && is_remote_locality(observation.locality);
  contribution.remote_access =
      is_remote_access_type(observation.type) || remote_locality;
  contribution.remote_read =
      observation.type == EventType::RemoteRead ||
      (remote_locality && observation.direction == AccessDirection::Read);
  contribution.remote_write =
      observation.type == EventType::RemoteWrite || observation.type == EventType::RemoteAtomic ||
      (remote_locality && (observation.direction == AccessDirection::Write ||
                           observation.direction == AccessDirection::ReadModifyWrite ||
                           observation.direction == AccessDirection::Atomic));
  contribution.ownership_transfer = is_ownership_event(observation.type);
  contribution.invalidation = observation.type == EventType::Invalidation ||
                              observation.type == EventType::RegionInvalidation;
  contribution.retry = observation.type == EventType::CoherenceRetry;
  contribution.conflict = observation.type == EventType::CoherenceConflict;
  contribution.writeback = observation.type == EventType::Writeback;
  contribution.counter_delta = observation.counter_delta.value_or(0);
  contribution.timestamp_ns = observation.timestamp_ns;
  contribution.precision = observation.precision;
  contribution.provenance = observation.provenance;
  return contribution;
}

bool AggregateStore::accumulate(const AggregateKey& key,
                                const ObservationContribution& contribution) {
  auto it = buckets_.find(key);
  if (it == buckets_.end()) {
    if (buckets_.size() >= Limits::kMaxAggregateKeys) {
      ++refused_updates_;
      return false;
    }
    it = buckets_.emplace(key, AggregateValue{}).first;
  }
  AggregateValue& bucket = it->second;

  bool overflowed_here = false;
  auto add = [&bucket, &overflowed_here](std::uint64_t& target, std::uint64_t amount) {
    const AddResult result = checked_add(target, amount);
    target = result.value;
    if (result.overflowed) {
      overflowed_here = true;
    }
  };

  add(bucket.observations, 1);
  add(bucket.bytes, contribution.bytes);
  add(bucket.lines, contribution.lines);
  add(bucket.pages, contribution.pages);
  add(bucket.regions_touched, contribution.regions_touched);
  add(bucket.counter_delta_total, contribution.counter_delta);
  if (contribution.remote_access) {
    add(bucket.remote_accesses, 1);
  }
  if (contribution.remote_read) {
    add(bucket.remote_reads, 1);
  }
  if (contribution.remote_write) {
    add(bucket.remote_writes, 1);
  }
  if (contribution.ownership_transfer) {
    add(bucket.ownership_transfers, 1);
  }
  if (contribution.invalidation) {
    add(bucket.invalidations, 1);
  }
  if (contribution.retry) {
    add(bucket.retries, 1);
  }
  if (contribution.conflict) {
    add(bucket.conflicts, 1);
  }
  if (contribution.writeback) {
    add(bucket.writebacks, 1);
  }

  if (!bucket.has_timestamps) {
    bucket.first_timestamp_ns = contribution.timestamp_ns;
    bucket.last_timestamp_ns = contribution.timestamp_ns;
    bucket.has_timestamps = true;
  } else {
    if (contribution.timestamp_ns < bucket.first_timestamp_ns) {
      bucket.first_timestamp_ns = contribution.timestamp_ns;
    }
    if (contribution.timestamp_ns > bucket.last_timestamp_ns) {
      bucket.last_timestamp_ns = contribution.timestamp_ns;
    }
  }

  const std::size_t precision_index = precision_rank(contribution.precision);
  if (precision_index < bucket.precision_counts.size()) {
    add(bucket.precision_counts[precision_index], 1);
  }
  const auto provenance_index = static_cast<std::size_t>(contribution.provenance);
  if (provenance_index < bucket.provenance_counts.size()) {
    add(bucket.provenance_counts[provenance_index], 1);
  }

  const Reality contribution_reality = reality_of(contribution.provenance);
  if (contribution_reality == Reality::Synthetic) {
    if (bucket.synthetic_contribution && bucket.observations > 1) {
      // Already synthetic; nothing to mix.
    } else if (bucket.has_observations && bucket.observations > 1) {
      bucket.mixed_contribution = true;
    }
    bucket.synthetic_contribution = true;
  } else if (contribution_reality == Reality::Mixed) {
    bucket.mixed_contribution = true;
    bucket.synthetic_contribution = true;
  } else if (bucket.synthetic_contribution) {
    bucket.mixed_contribution = true;
  }

  if (!bucket.has_observations) {
    bucket.weakest_precision = contribution.precision;
    bucket.has_observations = true;
  } else {
    bucket.weakest_precision = weakest(bucket.weakest_precision, contribution.precision);
  }

  if (overflowed_here) {
    bucket.saturated = true;
    ++saturations_;
  }
  return true;
}

const AggregateValue* AggregateStore::find(const AggregateKey& key) const noexcept {
  const auto it = buckets_.find(key);
  return it == buckets_.end() ? nullptr : &it->second;
}

std::uint64_t AggregateStore::observations_in(AggregateDimension dimension) const noexcept {
  std::uint64_t total = 0;
  for (const auto& entry : buckets_) {
    if (entry.first.dimension != dimension) {
      continue;
    }
    const AddResult result = checked_add(total, entry.second.observations);
    total = result.value;
    if (result.overflowed) {
      return total;
    }
  }
  return total;
}

std::uint64_t AggregateStore::bytes_in(AggregateDimension dimension) const noexcept {
  std::uint64_t total = 0;
  for (const auto& entry : buckets_) {
    if (entry.first.dimension != dimension) {
      continue;
    }
    const AddResult result = checked_add(total, entry.second.bytes);
    total = result.value;
    if (result.overflowed) {
      return total;
    }
  }
  return total;
}

void AggregateStore::restore(std::map<AggregateKey, AggregateValue> buckets) {
  buckets_ = std::move(buckets);
  saturations_ = 0;
  refused_updates_ = 0;
}

void AggregateStore::clear() noexcept {
  buckets_.clear();
  saturations_ = 0;
  refused_updates_ = 0;
}

}  // namespace sol::coherence
