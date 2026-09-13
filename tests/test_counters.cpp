// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "test_framework.hpp"
#include "test_support.hpp"

#include "coherence/aggregate.hpp"
#include "coherence/observation.hpp"
#include "coherence/observatory.hpp"

namespace {

using namespace sol::coherence;

CounterPublication make_publication(CounterKind kind, CounterScope scope, EventType type,
                                    std::uint32_t width, std::uint64_t generation,
                                    std::uint64_t raw, EventSequence sequence) {
  CounterPublication publication;
  publication.counter = CounterId{"ctr.test"};
  publication.mapped_type = type;
  publication.kind = kind;
  publication.scope = scope;
  publication.width_bits = width;
  publication.generation = CounterGeneration{generation};
  publication.sampling_epoch = SamplingEpoch{1};
  publication.raw_value = raw;
  publication.bytes_per_unit = 64;
  publication.sequence = sequence;
  publication.timestamp_ns = static_cast<Nanos>(sequence.value()) * 1000;
  publication.provenance = Provenance::HardwarePerformanceCounter;
  publication.granularity = scope == CounterScope::PerRegion ? EvidenceGranularity::Region
                                                            : EvidenceGranularity::Device;
  if (scope == CounterScope::PerRegion) {
    publication.region = MemoryRegionId{"region.test.0"};
    publication.region_generation = MemoryRegionGeneration{1};
  } else {
    publication.source = cotest::ref_accelerator("acc.a");
  }
  return publication;
}

CO_TEST(absolute_counter_produces_exact_deltas) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);

  const Result<CounterOutcome> first =
      host->publish_counter(make_publication(CounterKind::Absolute, CounterScope::PerDevice,
                                             EventType::RemoteRead, 64, 1, 1000,
                                             EventSequence{1}));
  CO_REQUIRE(first.ok());
  CO_CHECK_EQ(first.value().delta, std::uint64_t{0});
  CO_CHECK(!first.value().produced_delta);

  const Result<CounterOutcome> second =
      host->publish_counter(make_publication(CounterKind::Absolute, CounterScope::PerDevice,
                                             EventType::RemoteRead, 64, 1, 1100,
                                             EventSequence{2}));
  CO_REQUIRE(second.ok());
  CO_CHECK_EQ(second.value().delta, std::uint64_t{100});
  CO_CHECK(second.value().produced_delta);
  CO_CHECK(!second.value().discontinuity);

  const SnapshotPtr snapshot = observatory.snapshot();
  const AggregateValue* device =
      snapshot->aggregates().find(AggregateKey{AggregateDimension::Device, "acc.a"});
  CO_REQUIRE(device != nullptr);
  CO_CHECK_EQ(device->observations, std::uint64_t{1});
  CO_CHECK_EQ(device->bytes, std::uint64_t{100 * 64});
  CO_CHECK(device->weakest_precision == Precision::ExactCounterDelta);
}

CO_TEST(counter_reset_never_produces_negative_or_fabricated_traffic) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);

  host->publish_counter(make_publication(CounterKind::Resettable, CounterScope::PerDevice,
                                         EventType::RemoteRead, 64, 1, 5000, EventSequence{1}));
  const Result<CounterOutcome> second =
      host->publish_counter(make_publication(CounterKind::Resettable, CounterScope::PerDevice,
                                             EventType::RemoteRead, 64, 1, 7000,
                                             EventSequence{2}));
  CO_REQUIRE(second.ok());
  CO_CHECK_EQ(second.value().delta, std::uint64_t{2000});

  const Result<CounterOutcome> reset =
      host->publish_counter(make_publication(CounterKind::Resettable, CounterScope::PerDevice,
                                             EventType::RemoteRead, 64, 1, 4, EventSequence{3}));
  CO_REQUIRE(reset.ok());
  CO_CHECK_EQ(reset.value().delta, std::uint64_t{0});
  CO_CHECK(reset.value().discontinuity);
  CO_CHECK(reset.value().discontinuity_reason == "counter reset");

  const Result<CounterOutcome> after_reset =
      host->publish_counter(make_publication(CounterKind::Resettable, CounterScope::PerDevice,
                                             EventType::RemoteRead, 64, 1, 10, EventSequence{4}));
  CO_REQUIRE(after_reset.ok());
  CO_CHECK_EQ(after_reset.value().delta, std::uint64_t{6});

  const SnapshotPtr snapshot = observatory.snapshot();
  const AggregateValue* device =
      snapshot->aggregates().find(AggregateKey{AggregateDimension::Device, "acc.a"});
  CO_REQUIRE(device != nullptr);
  // Only the two positive deltas contributed; the reset contributed nothing.
  CO_CHECK_EQ(device->bytes, std::uint64_t{(2000 + 6) * 64});
}

CO_TEST(wrapping_counter_wraps_instead_of_resetting) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);

  const std::uint64_t near_max = 0xFFFFFF00ull;
  host->publish_counter(make_publication(CounterKind::Wrapping, CounterScope::PerDevice,
                                         EventType::RemoteWrite, 32, 1, near_max,
                                         EventSequence{1}));
  const Result<CounterOutcome> wrapped =
      host->publish_counter(make_publication(CounterKind::Wrapping, CounterScope::PerDevice,
                                             EventType::RemoteWrite, 32, 1, 0x100,
                                             EventSequence{2}));
  CO_REQUIRE(wrapped.ok());
  CO_CHECK(wrapped.value().discontinuity);
  CO_CHECK(wrapped.value().discontinuity_reason == "counter wrapped");
  // (max - last) + value + 1 crosses the wrap boundary inclusive of zero.
  CO_CHECK_EQ(wrapped.value().delta, std::uint64_t{(0xFFFFFFFFull - near_max) + 0x100 + 1});

  const Result<PublisherView> view = observatory.publisher(PublisherId{"pub.test.1"});
  CO_REQUIRE(view.ok());
  CO_CHECK_EQ(view.value().sequences.counter_wraps, std::uint64_t{1});
}

CO_TEST(counter_generation_change_breaks_delta_continuity) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);

  host->publish_counter(make_publication(CounterKind::Absolute, CounterScope::PerDevice,
                                         EventType::RemoteRead, 64, 1, 100, EventSequence{1}));
  host->publish_counter(make_publication(CounterKind::Absolute, CounterScope::PerDevice,
                                         EventType::RemoteRead, 64, 1, 200, EventSequence{2}));
  const std::uint64_t fingerprint_after_two = observatory.state_fingerprint();

  const Result<CounterOutcome> changed =
      host->publish_counter(make_publication(CounterKind::Absolute, CounterScope::PerDevice,
                                             EventType::RemoteRead, 64, 2, 50,
                                             EventSequence{3}));
  CO_REQUIRE(changed.ok());
  CO_CHECK(changed.value().discontinuity);
  CO_CHECK_EQ(changed.value().delta, std::uint64_t{0});
  CO_CHECK(fingerprint_after_two != observatory.state_fingerprint());

  const Result<CounterOutcome> resumed =
      host->publish_counter(make_publication(CounterKind::Absolute, CounterScope::PerDevice,
                                             EventType::RemoteRead, 64, 2, 90, EventSequence{4}));
  CO_REQUIRE(resumed.ok());
  CO_CHECK_EQ(resumed.value().delta, std::uint64_t{40});
}

CO_TEST(delta_and_sampled_counters) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);

  const Result<CounterOutcome> delta =
      host->publish_counter(make_publication(CounterKind::Delta, CounterScope::PerDevice,
                                             EventType::RemoteRead, 64, 1, 4096,
                                             EventSequence{1}));
  CO_REQUIRE(delta.ok());
  CO_CHECK_EQ(delta.value().delta, std::uint64_t{4096});

  const Result<CounterOutcome> sampled =
      host->publish_counter(make_publication(CounterKind::Sampled, CounterScope::PerDomain,
                                             EventType::RemoteRead, 64, 1, 9999,
                                             EventSequence{2}));
  CO_REQUIRE(sampled.ok());
  CO_CHECK_EQ(sampled.value().delta, std::uint64_t{0});
  CO_CHECK(!sampled.value().produced_delta);
}

CO_TEST(per_region_counter_is_attributable_to_a_region) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);

  host->publish_counter(make_publication(CounterKind::Delta, CounterScope::PerRegion,
                                         EventType::RemoteRead, 64, 1, 10, EventSequence{1}));
  const SnapshotPtr snapshot = observatory.snapshot();
  const AggregateValue* region =
      snapshot->aggregates().find(AggregateKey{AggregateDimension::Region, "region.test.0"});
  CO_REQUIRE(region != nullptr);
  CO_CHECK_EQ(region->observations, std::uint64_t{1});
  CO_CHECK_EQ(region->bytes, std::uint64_t{640});

  const Result<AttributionResult> attribution =
      observatory.attribute_region(MemoryRegionId{"region.test.0"});
  CO_REQUIRE(attribution.ok());
  CO_CHECK(attribution.value().outcome == AttributionOutcome::AttributedExact);
}

CO_TEST(counter_scope_cannot_be_inflated_to_a_region) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);

  CounterPublication publication =
      make_publication(CounterKind::Delta, CounterScope::PerDevice, EventType::RemoteRead, 64,
                       1, 100, EventSequence{1});
  publication.region = MemoryRegionId{"region.test.0"};
  publication.region_generation = MemoryRegionGeneration{1};
  const Result<CounterOutcome> outcome = host->publish_counter(publication);
  CO_CHECK(!outcome.ok());
  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::Region),
              std::uint64_t{0});
}

CO_TEST(counter_value_beyond_declared_width_is_rejected) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  const Result<CounterOutcome> outcome = host->publish_counter(
      make_publication(CounterKind::Absolute, CounterScope::PerDevice, EventType::RemoteRead, 8,
                       1, 100000, EventSequence{1}));
  CO_CHECK(!outcome.ok());
  CO_CHECK(outcome.code() == ErrorCode::OutOfRange);
}

CO_TEST(counter_discontinuity_is_reported_as_a_finding) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  host->publish_counter(make_publication(CounterKind::Resettable, CounterScope::PerDevice,
                                         EventType::RemoteRead, 64, 1, 5000,
                                         EventSequence{1}));
  host->publish_counter(make_publication(CounterKind::Resettable, CounterScope::PerDevice,
                                         EventType::RemoteRead, 64, 1, 1, EventSequence{2}));
  const Result<std::vector<Finding>> findings = observatory.findings();
  CO_REQUIRE(findings.ok());
  bool found = false;
  for (const Finding& finding : findings.value()) {
    if (finding.kind == FindingKind::CounterDiscontinuity) {
      found = true;
      bool has_reason = false;
      for (const std::string& reason : finding.reasons) {
        if (reason == finding_reason::kCounterReset) {
          has_reason = true;
        }
      }
      CO_CHECK(has_reason);
    }
  }
  CO_CHECK(found);
}

}  // namespace