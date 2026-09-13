// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <string>

#include "coherence/aggregate.hpp"
#include "coherence/ids.hpp"
#include "coherence/locality.hpp"
#include "coherence/observation.hpp"
#include "coherence/precision.hpp"
#include "coherence/provenance.hpp"
#include "coherence/taxonomy.hpp"

namespace {

using namespace sol::coherence;

CO_TEST(identity_token_validation) {
  CO_CHECK(validate_identity_token("pub.a-1_b:c@d#e+f").ok());
  CO_CHECK(!validate_identity_token("").ok());
  CO_CHECK(!validate_identity_token(std::string(Limits::kMaxNameLength + 1, 'a')).ok());
  CO_CHECK(!validate_identity_token("has space").ok());
  CO_CHECK(!validate_identity_token("has/slash").ok());
  CO_CHECK(!validate_identity_token(std::string("caf\xC3\xA9")).ok());
  CO_CHECK(!validate_identity_token("control\x01").ok());
}

CO_TEST(strong_ids_are_distinct_types) {
  const Result<PublisherId> publisher = PublisherId::parse("pub.a");
  CO_REQUIRE(publisher.ok());
  const Result<MemoryRegionId> region = MemoryRegionId::parse("region.a");
  CO_REQUIRE(region.ok());
  CO_CHECK(publisher.value().str() == "pub.a");
  CO_CHECK(region.value().str() == "region.a");
  CO_CHECK(publisher.value() == PublisherId{"pub.a"});
  CO_CHECK(!(publisher.value() == PublisherId{"pub.b"}));
  CO_CHECK(publisher.value() < PublisherId{"pub.b"});
  CO_CHECK(DeviceGeneration{1} < DeviceGeneration{2});
  CO_CHECK(DeviceGeneration{2}.next() == DeviceGeneration{3});
  CO_CHECK(PublisherBootId{7} == PublisherBootId{7});
  CO_CHECK(!(PublisherBootId{7} == PublisherBootId{8}));
  CO_CHECK(CoherenceEventId{0}.is_zero());
  CO_CHECK(std::hash<PublisherId>{}(PublisherId{"pub.a"}) ==
           std::hash<PublisherId>{}(PublisherId{"pub.a"}));
}

CO_TEST(precision_ordering_and_parsing) {
  CO_CHECK(precision_rank(Precision::ExactEvent) > precision_rank(Precision::ExactCounterDelta));
  CO_CHECK(precision_rank(Precision::ExactCounterDelta) >
           precision_rank(Precision::SampledEvent));
  CO_CHECK(precision_rank(Precision::SampledEvent) >
           precision_rank(Precision::AggregatedCounter));
  CO_CHECK(precision_rank(Precision::AggregatedCounter) > precision_rank(Precision::Derived));
  CO_CHECK(precision_rank(Precision::Derived) > precision_rank(Precision::Inferred));
  CO_CHECK(precision_rank(Precision::Inferred) > precision_rank(Precision::Unknown));
  CO_CHECK(weakest(Precision::ExactEvent, Precision::Inferred) == Precision::Inferred);
  CO_CHECK(weakest(Precision::ExactEvent, Precision::ExactCounterDelta) ==
           Precision::ExactCounterDelta);
  CO_CHECK(parse_precision("EXACT_EVENT").ok());
  CO_CHECK(parse_precision("exact_event").ok());
  CO_CHECK(!parse_precision("EXACT").ok());
  CO_CHECK(precision_within(Precision::Derived, Precision::ExactEvent));
  CO_CHECK(!precision_within(Precision::ExactEvent, Precision::Derived));
  const Precision values[] = {Precision::ExactEvent, Precision::Inferred};
  CO_CHECK(weakest_of(values, values + 2) == Precision::Inferred);
}

CO_TEST(provenance_and_reality) {
  CO_CHECK(is_real_provenance(Provenance::HardwarePerformanceCounter));
  CO_CHECK(is_real_provenance(Provenance::OsTelemetry));
  CO_CHECK(is_real_provenance(Provenance::ApplicationInstrumentation));
  CO_CHECK(!is_real_provenance(Provenance::SyntheticBackend));
  CO_CHECK(is_synthetic_provenance(Provenance::SyntheticBackend));
  CO_CHECK(reality_of(Provenance::SyntheticBackend) == Reality::Synthetic);
  CO_CHECK(reality_of(Provenance::HardwarePerformanceCounter) == Reality::Real);
  CO_CHECK(reality_of(Provenance::Unknown) == Reality::Mixed);
  CO_CHECK(combine_reality(Reality::Real, Reality::Real) == Reality::Real);
  CO_CHECK(combine_reality(Reality::Synthetic, Reality::Synthetic) == Reality::Synthetic);
  CO_CHECK(combine_reality(Reality::Real, Reality::Synthetic) == Reality::Mixed);
  std::vector<Capability> capabilities;
  Capability first;
  first.key = "b";
  first.status = CapabilityStatus::Unsupported;
  Capability second;
  second.key = "a";
  second.status = CapabilityStatus::Real;
  Capability duplicate;
  duplicate.key = "b";
  duplicate.status = CapabilityStatus::Real;
  capabilities = {first, second, duplicate};
  sort_capabilities(capabilities);
  CO_CHECK_EQ(capabilities.size(), std::size_t{2});
  CO_CHECK(capabilities[0].key == "a");
  CO_CHECK(capabilities[1].key == "b");
  // Under (key, status) ordering the lowest status ordinal is retained.
  CO_CHECK(capabilities[1].status == CapabilityStatus::Real);
}

CO_TEST(taxonomy_round_trip_and_unknown_handling) {
  for (std::uint8_t index = 0; index < kEventTypeCount; ++index) {
    const auto type = static_cast<EventType>(index);
    const Result<EventType> parsed = parse_event_type(to_string(type));
    CO_CHECK(parsed.ok());
    CO_CHECK(parsed.value() == type);
  }
  CO_CHECK(parse_event_type("unknown").value() == EventType::UnknownCoherenceEvent);
  CO_CHECK(!parse_event_type("NOT_A_CLASS").ok());
  CO_CHECK(is_ownership_event(EventType::OwnershipTransfer));
  CO_CHECK(is_ownership_event(EventType::MemoryDomainTransfer));
  CO_CHECK(!is_ownership_event(EventType::RemoteRead));
  CO_CHECK(is_traffic_event(EventType::RemoteWrite));
  CO_CHECK(!is_traffic_event(EventType::Invalidation));
  CO_CHECK(to_string(parse_locality("cxl_attached").value()) == "CXL_ATTACHED");
  CO_CHECK(!parse_locality("nonsense").ok());
  CO_CHECK(is_remote_locality(Locality::RemoteNuma));
  CO_CHECK(is_cxl_class_locality(Locality::PooledMemory));
  CO_CHECK(!is_cxl_class_locality(Locality::LocalNuma));
}

CO_TEST(bounded_metadata_enforces_its_bounds) {
  BoundedMetadata metadata;
  CO_CHECK(metadata.add("k", "v").ok());
  CO_CHECK(!metadata.add("k", "other").ok());
  CO_CHECK(!metadata.add("", "v").ok());
  CO_CHECK(!metadata.add(std::string(Limits::kMaxMetadataKeyLength + 1, 'k'), "v").ok());
  CO_CHECK(!metadata.add("k2", std::string(Limits::kMaxMetadataValueLength + 1, 'v')).ok());
  for (std::size_t i = metadata.size(); i < Limits::kMaxMetadataEntries; ++i) {
    CO_CHECK(metadata.add("key" + std::to_string(i), "value").ok());
  }
  CO_CHECK(!metadata.add("overflow", "value").ok());
  CO_CHECK(metadata.contains("k"));
  CO_CHECK(metadata.find("missing") == nullptr);
  const std::uint64_t first_hash = metadata.canonical_hash();
  BoundedMetadata reordered;
  for (const auto& entry : metadata.sorted_entries()) {
    reordered.add(entry.first, entry.second);
  }
  CO_CHECK(reordered.canonical_hash() == first_hash);
}

CO_TEST(observation_structure_rejects_malformed_records) {
  Observation valid;
  valid.event_id = CoherenceEventId{1};
  valid.type = EventType::RemoteRead;
  valid.source_publisher = PublisherId{"pub.test"};
  valid.publisher_boot = PublisherBootId{1};
  valid.coordinator_epoch = CoordinatorEpoch{1};
  valid.sequence = EventSequence{1};
  valid.timestamp_ns = 1000;
  valid.precision = Precision::ExactEvent;
  valid.granularity = EvidenceGranularity::Region;
  valid.provenance = Provenance::SyntheticBackend;
  CO_CHECK(validate_observation_structure(valid).ok());

  Observation zero_id = valid;
  zero_id.event_id = CoherenceEventId{0};
  CO_CHECK(!validate_observation_structure(zero_id).ok());

  Observation empty_publisher = valid;
  empty_publisher.source_publisher = PublisherId{};
  CO_CHECK(!validate_observation_structure(empty_publisher).ok());

  Observation zero_boot = valid;
  zero_boot.publisher_boot = PublisherBootId{0};
  CO_CHECK(!validate_observation_structure(zero_boot).ok());

  Observation negative_time = valid;
  negative_time.timestamp_ns = -1;
  CO_CHECK(!validate_observation_structure(negative_time).ok());

  Observation huge_bytes = valid;
  huge_bytes.bytes = Limits::kMaxByteCount + 1;
  CO_CHECK(!validate_observation_structure(huge_bytes).ok());

  Observation same_endpoints = valid;
  same_endpoints.source = cotest::ref_accelerator("acc.a");
  same_endpoints.target = cotest::ref_accelerator("acc.a");
  CO_CHECK(!validate_observation_structure(same_endpoints).ok());

  Observation unknown_kind = valid;
  ResourceRef ref;
  ref.kind = ResourceKind::Unknown;
  ref.id = ResourceId{"x"};
  unknown_kind.source = ref;
  CO_CHECK(!validate_observation_structure(unknown_kind).ok());

  Observation node_with_generation = valid;
  ResourceRef node_ref;
  node_ref.kind = ResourceKind::Node;
  node_ref.id = ResourceId{"node.test.0"};
  node_ref.generation = 4;
  node_with_generation.source = node_ref;
  CO_CHECK(!validate_observation_structure(node_with_generation).ok());

  Observation missing_region_generation = valid;
  missing_region_generation.region = MemoryRegionId{"region.test.0"};
  CO_CHECK(!validate_observation_structure(missing_region_generation).ok());

  Observation generation_without_region = valid;
  generation_without_region.region_generation = MemoryRegionGeneration{1};
  CO_CHECK(!validate_observation_structure(generation_without_region).ok());

  Observation exact_without_granularity = valid;
  exact_without_granularity.granularity = EvidenceGranularity::Unknown;
  CO_CHECK(!validate_observation_structure(exact_without_granularity).ok());

  Observation approximate_without_granularity = valid;
  approximate_without_granularity.granularity = EvidenceGranularity::Unknown;
  approximate_without_granularity.precision = Precision::Inferred;
  CO_CHECK(validate_observation_structure(approximate_without_granularity).ok());

  Observation counter_as_event = valid;
  counter_as_event.counter_delta = 10;
  counter_as_event.counter_generation = CounterGeneration{1};
  CO_CHECK(!validate_observation_structure(counter_as_event).ok());

  Observation delta_without_generation = valid;
  delta_without_generation.precision = Precision::ExactCounterDelta;
  delta_without_generation.counter_delta = 10;
  CO_CHECK(!validate_observation_structure(delta_without_generation).ok());

  Observation locality_without_flag = valid;
  locality_without_flag.locality = Locality::LocalNuma;
  CO_CHECK(!validate_observation_structure(locality_without_flag).ok());

  Observation unknown_without_detail = valid;
  unknown_without_detail.type = EventType::UnknownCoherenceEvent;
  CO_CHECK(!validate_observation_structure(unknown_without_detail).ok());
  CO_CHECK(unknown_without_detail.metadata.add("backend.event", "vendor-xyz").ok());
  CO_CHECK(validate_observation_structure(unknown_without_detail).ok());

  Observation bad_measured = valid;
  bad_measured.measured_duration_ns = -5;
  CO_CHECK(!validate_observation_structure(bad_measured).ok());
}

CO_TEST(counter_publication_structure) {
  CounterPublication valid;
  valid.publisher = PublisherId{"pub.test"};
  valid.publisher_boot = PublisherBootId{1};
  valid.coordinator_epoch = CoordinatorEpoch{1};
  valid.counter = CounterId{"ctr.a"};
  valid.mapped_type = EventType::RemoteRead;
  valid.kind = CounterKind::Absolute;
  valid.scope = CounterScope::PerDevice;
  valid.width_bits = 64;
  valid.generation = CounterGeneration{1};
  valid.sampling_epoch = SamplingEpoch{1};
  valid.sequence = EventSequence{1};
  valid.timestamp_ns = 100;
  valid.source = cotest::ref_accelerator("acc.a");
  valid.provenance = Provenance::HardwarePerformanceCounter;
  valid.granularity = EvidenceGranularity::Device;
  CO_CHECK(validate_counter_publication(valid).ok());

  CounterPublication unknown_scope = valid;
  unknown_scope.scope = CounterScope::Unknown;
  CO_CHECK(!validate_counter_publication(unknown_scope).ok());

  CounterPublication bad_width = valid;
  bad_width.width_bits = 0;
  CO_CHECK(!validate_counter_publication(bad_width).ok());

  CounterPublication coarse_with_region = valid;
  coarse_with_region.region = MemoryRegionId{"region.test.0"};
  coarse_with_region.region_generation = MemoryRegionGeneration{1};
  CO_CHECK(!validate_counter_publication(coarse_with_region).ok());

  CounterPublication per_region_without_region = valid;
  per_region_without_region.scope = CounterScope::PerRegion;
  CO_CHECK(!validate_counter_publication(per_region_without_region).ok());

  CounterPublication per_region_fine_granularity = valid;
  per_region_fine_granularity.scope = CounterScope::PerDevice;
  per_region_fine_granularity.granularity = EvidenceGranularity::CacheLine;
  CO_CHECK(!validate_counter_publication(per_region_fine_granularity).ok());

  CounterPublication device_without_source = valid;
  device_without_source.source.reset();
  CO_CHECK(!validate_counter_publication(device_without_source).ok());

  CounterPublication bad_type = valid;
  bad_type.mapped_type = static_cast<EventType>(200);
  CO_CHECK(!validate_counter_publication(bad_type).ok());
}

}  // namespace