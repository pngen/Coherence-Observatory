// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "test_framework.hpp"
#include "test_support.hpp"

#include "coherence/aggregate.hpp"
#include "coherence/observatory.hpp"

namespace {

using namespace sol::coherence;

CO_TEST(region_generation_change_makes_old_evidence_stale) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  host->publish(cotest::exact_observation(0, EventType::RemoteRead, 1000));

  RegionRecord replacement;
  replacement.id = MemoryRegionId{"region.test.0"};
  replacement.generation = MemoryRegionGeneration{2};
  replacement.owner = "test.workload";
  replacement.memory_domain = MemoryDomainId{"md.host.a"};
  replacement.memory_domain_generation = MemoryDomainGeneration{1};
  replacement.coherence_domain = CoherenceDomainId{"cd.test"};
  replacement.sharing_scope = SharingScope::DeviceShared;
  CO_REQUIRE(observatory.register_region(replacement).ok());

  const Result<IngestionOutcome> stale =
      host->publish(cotest::exact_observation(0, EventType::RemoteRead, 2000));
  CO_REQUIRE(stale.ok());
  CO_CHECK(stale.value().attribution == AttributionOutcome::StaleEvidence);
  CO_CHECK(!stale.value().counted);

  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::EventType),
              std::uint64_t{1});
  CO_CHECK(!snapshot->stale_evidence().empty());
  CO_CHECK(snapshot->stale_evidence().back().kind ==
           StaleEvidenceRecord::Kind::StaleGeneration);
}

CO_TEST(generation_rollback_is_refused) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  RegionRecord newer;
  newer.id = MemoryRegionId{"region.test.0"};
  newer.generation = MemoryRegionGeneration{3};
  CO_REQUIRE(observatory.register_region(newer).ok());
  RegionRecord older;
  older.id = MemoryRegionId{"region.test.0"};
  older.generation = MemoryRegionGeneration{2};
  const Status rollback = observatory.register_region(older);
  CO_CHECK(!rollback.ok());
  CO_CHECK(rollback.code() == ErrorCode::StaleGeneration);
}

CO_TEST(retired_region_cannot_receive_new_attribution) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  CO_REQUIRE(observatory
                 .retire_region(MemoryRegionId{"region.test.0"}, MemoryRegionGeneration{1},
                                "test retirement")
                 .ok());
  const Result<IngestionOutcome> outcome =
      host->publish(cotest::exact_observation(0, EventType::RemoteRead, 1000));
  CO_REQUIRE(outcome.ok());
  CO_CHECK(outcome.value().attribution == AttributionOutcome::StaleEvidence);
  CO_CHECK_EQ(observatory.snapshot()->aggregates().observations_in(
                  AggregateDimension::Region),
              std::uint64_t{0});

  // Retiring at a superseded generation is refused.
  const Status again = observatory.retire_region(MemoryRegionId{"region.test.0"},
                                                 MemoryRegionGeneration{9}, "wrong generation");
  CO_CHECK(!again.ok());
  CO_CHECK(again.code() == ErrorCode::StaleGeneration);
}

CO_TEST(topology_change_invalidates_locality_bound_evidence) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  host->publish(cotest::exact_observation(0, EventType::RemoteRead, 1000));

  const Result<TopologyGeneration> bumped =
      observatory.bump_topology_generation("test topology change");
  CO_REQUIRE(bumped.ok());
  CO_CHECK_EQ(bumped.value().value(), std::uint64_t{2});

  // Evidence that was produced under the previous topology states that
  // generation explicitly; the runtime does not silently re-tag it.
  Observation older = cotest::exact_observation(0, EventType::RemoteRead, 2000);
  older.topology_generation = TopologyGeneration{1};
  const Result<IngestionOutcome> stale = host->publish(older);
  CO_REQUIRE(stale.ok());
  CO_CHECK(stale.value().attribution == AttributionOutcome::StaleEvidence);
  CO_CHECK(stale.value().attribution_reason == attribution_reason::kStaleTopology);

  // Fresh evidence is bound to the topology actually in force.
  const Result<IngestionOutcome> fresh =
      host->publish(cotest::exact_observation(0, EventType::RemoteRead, 3000));
  CO_REQUIRE(fresh.ok());
  CO_CHECK(fresh.value().attribution != AttributionOutcome::StaleEvidence);
  // A named snapshot keeps the shared_ptr alive; a temporary would dangle the
  // reference returned by stale_evidence().
  const SnapshotPtr snapshot = observatory.snapshot();
  bool found_topology_stale = false;
  for (const StaleEvidenceRecord& record : snapshot->stale_evidence()) {
    if (record.kind == StaleEvidenceRecord::Kind::TopologySuperseded) {
      found_topology_stale = true;
    }
  }
  CO_CHECK(found_topology_stale);
}

CO_TEST(device_generation_change_makes_old_evidence_stale) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  host->publish(cotest::exact_observation(0, EventType::RemoteRead, 1000));

  AcceleratorRecord replacement;
  replacement.id = AcceleratorId{"acc.a"};
  replacement.node = sol::coherence::NodeId{"node.test.0"};
  replacement.generation = DeviceGeneration{2};
  replacement.vendor = "synthetic";
  replacement.vendor_uuid = "acc.a-uuid";
  replacement.local_memory_domain = MemoryDomainId{"md.acc.a"};
  CO_REQUIRE(observatory.register_accelerator(replacement).ok());

  const Result<IngestionOutcome> stale =
      host->publish(cotest::exact_observation(0, EventType::RemoteRead, 2000));
  CO_REQUIRE(stale.ok());
  CO_CHECK(stale.value().attribution == AttributionOutcome::StaleEvidence);
  CO_CHECK(stale.value().attribution_reason == attribution_reason::kStaleRegionGeneration);
}

CO_TEST(identity_change_without_a_generation_change_is_a_conflict) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  AcceleratorRecord changed;
  changed.id = AcceleratorId{"acc.a"};
  changed.node = sol::coherence::NodeId{"node.test.0"};
  changed.generation = DeviceGeneration{1};
  changed.vendor = "synthetic";
  changed.vendor_uuid = "different-uuid";
  changed.local_memory_domain = MemoryDomainId{"md.acc.a"};
  const Status status = observatory.register_accelerator(changed);
  CO_CHECK(!status.ok());
  CO_CHECK(status.code() == ErrorCode::Conflict);
}

CO_TEST(fenced_boot_loses_authority_permanently) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  const PublisherBootId boot = host->publisher_boot();

  host->publish(cotest::exact_observation(0, EventType::RemoteRead, 1000));
  CO_REQUIRE(observatory
                 .fence_publisher_boot(PublisherId{"pub.test.1"}, boot,
                                       FenceReason::ConnectionClosed, "test")
                 .ok());
  const Result<IngestionOutcome> rejected =
      host->publish(cotest::exact_observation(0, EventType::RemoteRead, 2000));
  CO_REQUIRE(rejected.ok());
  CO_CHECK(rejected.value().disposition == IngestionDisposition::Rejected);
  CO_CHECK(rejected.value().code == ErrorCode::StaleBoot);

  PublisherRegistration replay;
  replay.id = PublisherId{"pub.test.1"};
  replay.boot = boot;
  const Result<PublisherView> refused = observatory.register_publisher(replay);
  CO_CHECK(!refused.ok());
  CO_CHECK(refused.code() == ErrorCode::StaleBoot);
}

CO_TEST(publisher_reincarnation_requires_a_fresh_boot) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto first = cotest::start_host(observatory, "pub.test.1");
  const PublisherBootId first_boot = first->publisher_boot();
  first->publish(cotest::exact_observation(0, EventType::RemoteRead, 1000));

  PublisherRegistration replacement;
  replacement.id = PublisherId{"pub.test.1"};
  replacement.boot = PublisherBootId{first_boot.value() + 1};
  const Result<PublisherView> view = observatory.register_publisher(replacement);
  CO_REQUIRE(view.ok());
  CO_CHECK(view.value().current);
  CO_CHECK_EQ(view.value().boot.value(), first_boot.value() + 1);

  // The superseded boot is now permanently rejected.
  const Result<IngestionOutcome> stale =
      first->publish(cotest::exact_observation(0, EventType::RemoteRead, 2000));
  CO_REQUIRE(stale.ok());
  CO_CHECK(stale.value().code == ErrorCode::StaleBoot);
}

CO_TEST(coordinator_epoch_mismatch_is_rejected_without_mutation) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  const std::uint64_t fingerprint = observatory.state_fingerprint();

  Observation observation = cotest::exact_observation(0, EventType::RemoteRead, 1000);
  observation.source_publisher = host->publisher_id();
  observation.publisher_boot = host->publisher_boot();
  observation.event_id = CoherenceEventId{1};
  observation.sequence = EventSequence{1};
  observation.coordinator_epoch = CoordinatorEpoch{99};
  const Result<IngestionOutcome> outcome = observatory.ingest(observation);
  CO_REQUIRE(outcome.ok());
  CO_CHECK(outcome.value().code == ErrorCode::StaleEpoch);
  CO_CHECK_EQ(observatory.state_fingerprint(), fingerprint);
  CO_CHECK(host->rejected_observations() == 0);
}

CO_TEST(reconciliation_advances_the_observation_epoch) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  host->publish(cotest::exact_observation(0, EventType::RemoteRead, 1000));

  const ObservationEpoch before = observatory.observation_epoch();
  const Result<ReconcileReport> report = observatory.reconcile_current_evidence();
  CO_REQUIRE(report.ok());
  CO_CHECK(observatory.observation_epoch().value() > before.value());
  CO_CHECK_EQ(report.value().evidence_generations_retired, std::uint64_t{1});
}

CO_TEST(stale_evidence_is_labelled_not_silently_dropped) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  CO_REQUIRE(observatory
                 .retire_region(MemoryRegionId{"region.test.1"}, MemoryRegionGeneration{1},
                                "retire for test")
                 .ok());
  host->publish(cotest::exact_observation(1, EventType::Invalidation, 1000));
  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK(!snapshot->stale_evidence().empty());
  CO_CHECK(snapshot->stale_evidence().back().kind ==
           StaleEvidenceRecord::Kind::RetiredRegion);
  CO_CHECK(!snapshot->stale_evidence().back().detail.empty());
}

}  // namespace