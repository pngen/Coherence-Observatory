// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <limits>

#include "coherence/aggregate.hpp"
#include "coherence/checked.hpp"
#include "coherence/observatory.hpp"

namespace {

using namespace sol::coherence;

CO_TEST(checked_arithmetic_reports_overflow) {
  // Values are read through volatile storage so that the checked helpers are
  // exercised at run time rather than folded into constant expressions.
  volatile std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max();
  const std::uint64_t max_value = maximum;
  const AddResult simple = checked_add(2, 3);
  CO_CHECK(!simple.overflowed);
  CO_CHECK_EQ(simple.value, std::uint64_t{5});
  const AddResult overflow = checked_add(max_value, 1);
  CO_CHECK(overflow.overflowed);
  CO_CHECK_EQ(overflow.value, max_value);
  CO_CHECK(!add_fits(max_value, 1));
  const AddResult product = checked_mul(1ull << 40, 1ull << 40);
  CO_CHECK(product.overflowed);
  const AddResult capped = checked_add_capped(10, 10, 15);
  CO_CHECK(capped.overflowed);
  CO_CHECK_EQ(capped.value, std::uint64_t{15});
}

CO_TEST(aggregate_store_saturates_and_reports) {
  AggregateStore store;
  AggregateKey key;
  key.dimension = AggregateDimension::Region;
  key.value = "region.x";
  ObservationContribution contribution;
  contribution.bytes = std::numeric_limits<std::uint64_t>::max();
  CO_CHECK(store.accumulate(key, contribution));
  CO_CHECK(store.accumulate(key, contribution));
  const AggregateValue* bucket = store.find(key);
  CO_REQUIRE(bucket != nullptr);
  CO_CHECK(bucket->saturated);
  CO_CHECK_EQ(bucket->bytes, std::numeric_limits<std::uint64_t>::max());
  CO_CHECK(store.saturations() > 0);
}

CO_TEST(aggregate_store_refuses_updates_at_capacity) {
  AggregateStore store;
  ObservationContribution contribution;
  std::size_t inserted = 0;
  for (std::size_t i = 0; i <= Limits::kMaxAggregateKeys + 1; ++i) {
    AggregateKey key;
    key.dimension = AggregateDimension::EventType;
    key.value = "value." + std::to_string(i);
    if (!store.accumulate(key, contribution)) {
      break;
    }
    ++inserted;
  }
  CO_CHECK_EQ(inserted, Limits::kMaxAggregateKeys);
  CO_CHECK(store.refused_updates() > 0);
}

CO_TEST(aggregation_is_deterministic) {
  auto run = []() {
    Observatory observatory(cotest::test_options());
    cotest::register_test_topology(observatory);
    auto host = cotest::start_host(observatory);
    for (std::uint64_t i = 1; i <= 64; ++i) {
      Observation observation =
          cotest::exact_observation(static_cast<std::size_t>(i % 4), EventType::RemoteRead,
                                    static_cast<Nanos>(i * 100));
      observation.sequence = EventSequence{i};
      host->publish(observation);
    }
    return observatory.state_fingerprint();
  };
  CO_CHECK_EQ(run(), run());
}

CO_TEST(aggregate_dimension_text_is_canonical) {
  Observation observation = cotest::exact_observation(0, EventType::RemoteRead, 1000);
  CO_CHECK(aggregate_value_text(observation, AggregateDimension::EventType, 1000) ==
           "REMOTE_READ");
  CO_CHECK(aggregate_value_text(observation, AggregateDimension::Region, 1000) ==
           "region.test.0");
  CO_CHECK(aggregate_value_text(observation, AggregateDimension::Device, 1000) == "acc.a");
  CO_CHECK(aggregate_value_text(observation, AggregateDimension::Domain, 1000) == "md.host.a");
  CO_CHECK(aggregate_value_text(observation, AggregateDimension::Workload, 1000) == "unknown");
  CO_CHECK(aggregate_value_text(observation, AggregateDimension::Locality, 1000) == "unknown");
  CO_CHECK(aggregate_value_text(observation, AggregateDimension::TimeBucket, 1000) == "1");
  CO_CHECK(aggregate_value_text(observation, AggregateDimension::TimeBucket, 0) == "unknown");
  CO_CHECK(aggregate_value_text(observation, AggregateDimension::SourceTargetPair, 1000) ==
           "acc.a->md.host.a");
}

CO_TEST(precision_and_reality_are_preserved_per_bucket) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.synthetic.1");
  auto real_host = cotest::start_host(observatory, "pub.real.1", Provenance::OsTelemetry);

  Observation synthetic = cotest::exact_observation(0, EventType::RemoteRead, 1000);
  synthetic.provenance = Provenance::SyntheticBackend;
  host->publish(synthetic);

  Observation real = cotest::exact_observation(0, EventType::RemoteRead, 2000);
  real.provenance = Provenance::OsTelemetry;
  real.sequence = EventSequence{1};
  real_host->publish(real);

  const SnapshotPtr snapshot = observatory.snapshot();
  const AggregateValue* region =
      snapshot->aggregates().find(AggregateKey{AggregateDimension::Region, "region.test.0"});
  CO_REQUIRE(region != nullptr);
  CO_CHECK_EQ(region->observations, std::uint64_t{2});
  CO_CHECK(region->reality() == Reality::Mixed);
  CO_CHECK(region->weakest_precision == Precision::ExactEvent);
  CO_CHECK(region->provenance_counts[static_cast<std::size_t>(
                Provenance::SyntheticBackend)] == 1);
  CO_CHECK(region->provenance_counts[static_cast<std::size_t>(Provenance::OsTelemetry)] == 1);
}

CO_TEST(observation_journal_is_bounded_and_loss_is_reported) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  const std::size_t total = Limits::kMaxObservationJournal + 32;
  for (std::size_t i = 0; i < total; ++i) {
    host->publish(cotest::exact_observation(i % 4, EventType::RemoteRead,
                                            static_cast<Nanos>(i * 10)));
  }
  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::EventType),
              static_cast<std::uint64_t>(total));
  CO_CHECK(snapshot->loss().loss.dropped_journal_entries != 0);
  bool has_capacity_finding = false;
  for (const Finding& finding : snapshot->findings()) {
    if (finding.kind == FindingKind::CapacityPressure) {
      has_capacity_finding = true;
    }
  }
  CO_CHECK(has_capacity_finding);
}

CO_TEST(per_region_sample_window_is_bounded) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  const std::size_t total = Limits::kMaxRegionSamples + 64;
  for (std::size_t i = 0; i < total; ++i) {
    host->publish(cotest::exact_observation(0, EventType::OwnershipTransfer,
                                            static_cast<Nanos>(i * 10)));
  }
  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK(snapshot->loss().loss.dropped_region_samples >= 64);
}

}  // namespace