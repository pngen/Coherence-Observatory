// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Exact bounded aggregation.
//
// Aggregation uses checked arithmetic throughout.  Overflow saturates and sets
// an explicit flag that is reported all the way to the CLI: the runtime never
// silently loses a count.

#ifndef COHERENCE_AGGREGATE_HPP
#define COHERENCE_AGGREGATE_HPP

#include <array>
#include <compare>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "coherence/limits.hpp"
#include "coherence/locality.hpp"
#include "coherence/observation.hpp"
#include "coherence/precision.hpp"
#include "coherence/provenance.hpp"
#include "coherence/taxonomy.hpp"
#include "coherence/time.hpp"

namespace sol::coherence {

/// Dimensions along which observations are aggregated.
enum class AggregateDimension : std::uint8_t {
  EventType = 0,
  Device = 1,
  Domain = 2,
  Region = 3,
  Workload = 4,
  Locality = 5,
  TimeBucket = 6,
  Publisher = 7,
  CoherenceDomain = 8,
  SourceTargetPair = 9,

  kCount = 10,
};

inline constexpr std::size_t kAggregateDimensionCount = 10;

COHERENCE_API std::string_view to_string(AggregateDimension dimension) noexcept;

/// A single aggregation bucket key.  c value is always the canonical text
/// form of the dimension value, so buckets are comparable and serializable.
struct COHERENCE_API AggregateKey {
  AggregateDimension dimension = AggregateDimension::EventType;
  std::string value;

  friend bool operator==(const AggregateKey&, const AggregateKey&) = default;
  friend auto operator<=>(const AggregateKey&, const AggregateKey&) = default;
};

/// Accumulated values for one bucket.
struct COHERENCE_API AggregateValue {
  std::uint64_t observations = 0;
  std::uint64_t bytes = 0;
  std::uint64_t lines = 0;
  std::uint64_t pages = 0;
  std::uint64_t regions_touched = 0;
  std::uint64_t remote_accesses = 0;
  std::uint64_t remote_reads = 0;
  std::uint64_t remote_writes = 0;
  std::uint64_t ownership_transfers = 0;
  std::uint64_t invalidations = 0;
  std::uint64_t retries = 0;
  std::uint64_t conflicts = 0;
  std::uint64_t writebacks = 0;
  std::uint64_t counter_delta_total = 0;

  Nanos first_timestamp_ns = 0;
  Nanos last_timestamp_ns = 0;
  bool has_timestamps = false;

  /// Per-precision observation counts, indexed by Precision rank.
  std::array<std::uint64_t, 7> precision_counts{};
  /// Per-provenance observation counts, indexed by Provenance value (0..9).
  std::array<std::uint64_t, 10> provenance_counts{};

  /// True when any accumulation saturated; totals are then lower bounds.
  bool saturated = false;
  /// True when at least one contributing observation was SYNTHETIC.
  bool synthetic_contribution = false;
  /// True when contributions are mixed real/synthetic.
  bool mixed_contribution = false;

  /// Weakest precision among contributing observations.
  Precision weakest_precision = Precision::ExactEvent;
  bool has_observations = false;

  Reality reality() const noexcept;
  double rate_per_second() const noexcept;
};

/// The contribution one observation makes to a bucket.
struct COHERENCE_API ObservationContribution {
  std::uint64_t bytes = 0;
  std::uint64_t lines = 0;
  std::uint64_t pages = 0;
  std::uint64_t regions_touched = 0;
  bool remote_access = false;
  bool remote_read = false;
  bool remote_write = false;
  bool ownership_transfer = false;
  bool invalidation = false;
  bool retry = false;
  bool conflict = false;
  bool writeback = false;
  std::uint64_t counter_delta = 0;
  Nanos timestamp_ns = 0;
  Precision precision = Precision::Unknown;
  Provenance provenance = Provenance::Unknown;
};

/// Bounded, deterministic aggregate store.
///
/// Iteration order is the key ordering, which makes rendering reproducible.
class COHERENCE_API AggregateStore {
 public:
  /// Accumulates one observation's contribution into p dimension bucket.
  ///
  /// Returns false when the update could not be applied because the store is
  /// at capacity; the caller must record the loss explicitly.
  bool accumulate(const AggregateKey& key, const ObservationContribution& contribution);

  std::size_t size() const noexcept { return buckets_.size(); }
  bool empty() const noexcept { return buckets_.empty(); }
  std::size_t capacity() const noexcept { return Limits::kMaxAggregateKeys; }

  const std::map<AggregateKey, AggregateValue>& buckets() const noexcept { return buckets_; }

  /// Returns the bucket for p key or nullptr.
  const AggregateValue* find(const AggregateKey& key) const noexcept;

  /// Sum of c observations across a dimension's buckets.
  std::uint64_t observations_in(AggregateDimension dimension) const noexcept;

  /// Sum of c bytes across a dimension's buckets.
  std::uint64_t bytes_in(AggregateDimension dimension) const noexcept;

  /// Total saturations recorded while accumulating.
  std::uint64_t saturations() const noexcept { return saturations_; }
  /// Total updates refused because the store was full.
  std::uint64_t refused_updates() const noexcept { return refused_updates_; }

  void clear() noexcept;

  /// Replaces the bucket set verbatim.  Used only by durable recovery, which
  /// must restore recorded totals exactly rather than recompute them.
  void restore(std::map<AggregateKey, AggregateValue> buckets);

 private:
  std::map<AggregateKey, AggregateValue> buckets_;
  std::uint64_t saturations_ = 0;
  std::uint64_t refused_updates_ = 0;
};

/// Builds the contribution implied by an observation.
COHERENCE_API ObservationContribution contribution_of(const Observation& observation);

/// Canonical bucket value text for an observation on p dimension.
///
/// Dimensions the observation cannot address render as "unknown": the runtime
/// never invents a bucket value.
COHERENCE_API std::string aggregate_value_text(const Observation& observation,
                                               AggregateDimension dimension,
                                               std::int64_t time_bucket_ns);

/// Canonical bucket value text for a resource reference.
COHERENCE_API std::string resource_aggregate_text(const ResourceRef& ref);

}  // namespace sol::coherence

#endif  // COHERENCE_AGGREGATE_HPP
