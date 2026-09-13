// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "test_framework.hpp"
#include "test_support.hpp"

#include "coherence/observatory.hpp"

namespace {

using namespace sol::coherence;

/// Builds a well-formed observation that is not published, so that
/// Observatory::attribute can be exercised directly.
Observation base(Observatory& observatory, EventType type) {
  Observation observation;
  observation.type = type;
  observation.timestamp_ns = 1000;
  observation.direction = AccessDirection::Read;
  observation.bytes = 64;
  observation.lines = 1;
  observation.source = cotest::ref_accelerator("acc.a");
  observation.target = cotest::ref_domain("md.host.a");
  observation.precision = Precision::ExactEvent;
  observation.granularity = EvidenceGranularity::Region;
  observation.provenance = Provenance::SyntheticBackend;
  return cotest::authorized(observatory, "pub.test.1", 1, observation);
}

CO_TEST(exact_per_region_attribution) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  host->publish(cotest::exact_observation(0, EventType::RemoteRead, 1000));

  const Result<AttributionResult> attribution =
      observatory.attribute_region(MemoryRegionId{"region.test.0"});
  CO_REQUIRE(attribution.ok());
  CO_CHECK(attribution.value().outcome == AttributionOutcome::AttributedExact);
  CO_CHECK(attribution.value().bindings.region_generation_bound);
  CO_CHECK_EQ(attribution.value().bindings.region_generation.value(), std::uint64_t{1});
  CO_CHECK(attribution.value().precision == Precision::ExactEvent);
  CO_CHECK(attribution.value().reality == Reality::Synthetic);
  bool has_region_target = false;
  for (const AttributionTarget& target : attribution.value().targets) {
    if (target.kind == AttributionTargetKind::Region) {
      has_region_target = true;
    }
  }
  CO_CHECK(has_region_target);
}

CO_TEST(partial_attribution_without_a_region) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  Observation observation = base(observatory, EventType::SharedRead);
  const Result<AttributionResult> attribution = observatory.attribute(observation);
  CO_REQUIRE(attribution.ok());
  CO_CHECK(attribution.value().outcome == AttributionOutcome::AttributedPartial);
  CO_CHECK(attribution.value().has_reason(attribution_reason::kNoRegionIdentified));
}

CO_TEST(coarse_granularity_downgrades_a_region_claim) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  Observation observation = base(observatory, EventType::RemoteRead);
  observation.granularity = EvidenceGranularity::Device;
  observation.precision = Precision::ExactCounterDelta;
  observation.counter_delta = 512;
  observation.counter_generation = CounterGeneration{1};
  observation.region = MemoryRegionId{"region.test.0"};
  observation.region_generation = MemoryRegionGeneration{1};
  const Result<AttributionResult> attribution = observatory.attribute(observation);
  CO_REQUIRE(attribution.ok());
  CO_CHECK(attribution.value().outcome == AttributionOutcome::AttributedAggregateOnly);
  CO_CHECK(attribution.value().has_reason(attribution_reason::kCoarseGranularity));
  bool has_region_target = false;
  for (const AttributionTarget& target : attribution.value().targets) {
    if (target.kind == AttributionTargetKind::Region) {
      has_region_target = true;
    }
  }
  CO_CHECK(!has_region_target);
}

CO_TEST(ambiguous_binding_when_several_regions_are_candidates) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  Observation observation = base(observatory, EventType::RemoteRead);
  observation.workload = WorkloadId{"workload.test"};
  const Result<AttributionResult> attribution = observatory.attribute(observation);
  CO_REQUIRE(attribution.ok());
  CO_CHECK(attribution.value().outcome == AttributionOutcome::Ambiguous);
  CO_CHECK(attribution.value().candidate_count >= 2);
  CO_CHECK(!attribution.value().candidates.empty());
}

CO_TEST(unattributed_when_nothing_can_be_bound) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  Observation observation;
  observation.type = EventType::RemoteRead;
  observation.timestamp_ns = 1000;
  observation.precision = Precision::Inferred;
  observation.provenance = Provenance::SyntheticBackend;
  observation = cotest::authorized(observatory, "pub.test.1", 1, observation);
  const Result<AttributionResult> attribution = observatory.attribute(observation);
  CO_REQUIRE(attribution.ok());
  CO_CHECK(attribution.value().outcome == AttributionOutcome::Unattributed);
  CO_CHECK(attribution.value().has_reason(attribution_reason::kNoSubject));
}

CO_TEST(stale_evidence_outcome_for_a_fenced_publisher) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  const Observation observation = cotest::exact_observation(0, EventType::RemoteRead, 1000);
  host->publish(observation);
  observatory.fence_publisher_boot(PublisherId{"pub.test.1"}, host->publisher_boot(),
                                   FenceReason::Administrative, "test");
  const Observation attempted =
      cotest::authorized(observatory, "pub.test.1", 2, observation);
  const Result<AttributionResult> attribution = observatory.attribute(attempted);
  CO_REQUIRE(attribution.ok());
  CO_CHECK(attribution.value().outcome == AttributionOutcome::StaleEvidence);
  CO_CHECK(attribution.value().has_reason(attribution_reason::kStalePublisher));
}

CO_TEST(unsupported_observability_is_reported_not_guessed) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.real.1", Provenance::OsTelemetry);

  Observation observation = cotest::exact_observation(0, EventType::Invalidation, 1000);
  observation.provenance = Provenance::OsTelemetry;
  observation.granularity = EvidenceGranularity::CacheLine;
  observation.precision = Precision::ExactEvent;
  const Result<IngestionOutcome> outcome = host->publish(observation);
  CO_REQUIRE(outcome.ok());
  CO_CHECK(outcome.value().attribution == AttributionOutcome::Unsupported);
  CO_CHECK(outcome.value().attribution_reason == attribution_reason::kUnsupportedCapability);
  CO_CHECK(!outcome.value().counted);
}

CO_TEST(cxl_locality_cannot_be_claimed_without_telemetry) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.real.2", Provenance::OsTelemetry);

  Observation observation = cotest::exact_observation(0, EventType::RemoteRead, 1000);
  observation.provenance = Provenance::OsTelemetry;
  observation.source = cotest::ref_processor("cpu.a");
  observation.target = cotest::ref_domain("md.cxl");
  observation.locality = Locality::CxlAttached;
  observation.locality_declared = true;
  const Result<IngestionOutcome> outcome = host->publish(observation);
  CO_REQUIRE(outcome.ok());
  CO_CHECK(outcome.value().attribution == AttributionOutcome::Unsupported);
}

CO_TEST(locality_is_derived_from_the_registered_topology) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  Observation observation = base(observatory, EventType::RemoteRead);
  observation.source = cotest::ref_accelerator("acc.b");
  const Result<AttributionResult> attribution = observatory.attribute(observation);
  CO_REQUIRE(attribution.ok());
  CO_CHECK(attribution.value().locality_established);
  CO_CHECK(attribution.value().locality == Locality::PeerAccelerator);
  CO_CHECK(attribution.value().has_reason(attribution_reason::kBoundToEdge));
}

CO_TEST(conflicting_locality_is_reported_as_ambiguity) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  Observation observation = base(observatory, EventType::RemoteRead);
  observation.source = cotest::ref_accelerator("acc.b");
  observation.locality = Locality::RemoteNode;
  observation.locality_declared = true;
  const Result<AttributionResult> attribution = observatory.attribute(observation);
  CO_REQUIRE(attribution.ok());
  CO_CHECK(attribution.value().has_reason(attribution_reason::kConflictingLocality));
  CO_CHECK(!attribution.value().locality_established);
  CO_CHECK_EQ(attribution.value().candidate_count, std::uint64_t{2});
}

CO_TEST(attribution_never_exceeds_evidence_precision) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  const Precision precisions[] = {Precision::ExactEvent, Precision::ExactCounterDelta,
                                  Precision::SampledEvent, Precision::AggregatedCounter,
                                  Precision::Derived, Precision::Inferred};
  std::uint64_t sequence = 1;
  for (Precision precision : precisions) {
    Observation observation = base(observatory, EventType::RemoteRead);
    observation.precision = precision;
    observation = cotest::authorized(observatory, "pub.test.1", sequence++, observation);
    if (precision == Precision::ExactCounterDelta) {
      observation.counter_delta = 10;
      observation.counter_generation = CounterGeneration{1};
    }
    const Result<AttributionResult> attribution = observatory.attribute(observation);
    CO_REQUIRE(attribution.ok());
    CO_CHECK(precision_rank(attribution.value().precision) <= precision_rank(precision));
  }
}

CO_TEST(unknown_resource_generation_is_stale_not_exact) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  Observation observation = base(observatory, EventType::RemoteRead);
  ResourceRef future = cotest::ref_accelerator("acc.a");
  future.generation = 7;
  observation.source = future;
  const Result<AttributionResult> attribution = observatory.attribute(observation);
  CO_REQUIRE(attribution.ok());
  CO_CHECK(attribution.value().outcome == AttributionOutcome::StaleEvidence);
  CO_CHECK(attribution.value().has_reason(attribution_reason::kStaleRegionGeneration));
}

}  // namespace