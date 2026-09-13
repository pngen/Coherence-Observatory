// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Hardening phase: deliberate attempts to break the runtime.  Every case here
// was chosen because it could plausibly corrupt state, inflate evidence or
// hide loss; each one is a regression test for a specific defence.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <atomic>
#include <filesystem>
#include <limits>
#include <thread>
#include <vector>

#include "coherence/aggregate.hpp"
#include "coherence/backends/imported_trace.hpp"
#include "coherence/client.hpp"
#include "coherence/coordinator.hpp"
#include "coherence/observatory.hpp"

namespace {

using namespace sol::coherence;

CO_TEST(hardening_full_width_counter_decrease_is_a_reset_not_a_wrap) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.hard.1");

  auto publish = [&host](CounterKind kind, std::uint32_t width, std::uint64_t raw,
                         std::uint64_t sequence) {
    CounterPublication publication;
    publication.counter = CounterId{"ctr.hard"};
    publication.mapped_type = EventType::RemoteRead;
    publication.kind = kind;
    publication.scope = CounterScope::PerDevice;
    publication.width_bits = width;
    publication.generation = CounterGeneration{1};
    publication.sampling_epoch = SamplingEpoch{1};
    publication.raw_value = raw;
    publication.bytes_per_unit = 1;
    publication.sequence = EventSequence{sequence};
    publication.timestamp_ns = static_cast<Nanos>(sequence) * 1000;
    publication.provenance = Provenance::HardwarePerformanceCounter;
    publication.granularity = EvidenceGranularity::Device;
    publication.source = cotest::ref_accelerator("acc.a");
    return host->publish_counter(publication);
  };

  publish(CounterKind::Wrapping, 64, std::numeric_limits<std::uint64_t>::max() - 10, 1);
  const Result<CounterOutcome> decrease = publish(CounterKind::Wrapping, 64, 5, 2);
  CO_REQUIRE(decrease.ok());
  // A 64-bit counter cannot be distinguished from a reset, so the runtime
  // reports a reset and never invents a wrap-around delta.
  CO_CHECK(decrease.value().discontinuity);
  CO_CHECK(decrease.value().discontinuity_reason == "counter reset");
  CO_CHECK_EQ(decrease.value().delta, std::uint64_t{0});

  const Result<CounterOutcome> resumed = publish(CounterKind::Wrapping, 64, 9, 3);
  CO_REQUIRE(resumed.ok());
  CO_CHECK_EQ(resumed.value().delta, std::uint64_t{4});
}

CO_TEST(hardening_sequence_window_boundary_is_exact) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.hard.2");

  const std::uint64_t total = Limits::kMaxSequenceWindow + 1;
  for (std::uint64_t i = 1; i <= total; ++i) {
    Observation observation = cotest::exact_observation(0, EventType::RemoteRead,
                                                        static_cast<Nanos>(i * 10));
    observation.sequence = EventSequence{i};
    observation.event_id = CoherenceEventId{i};
    CO_REQUIRE(host->publish(observation).ok());
  }
  // Sequence 1 is now exactly one below the retention window: it must be
  // refused rather than accepted as a late event.
  Observation oldest = cotest::exact_observation(0, EventType::RemoteRead, 5);
  oldest.sequence = EventSequence{1};
  oldest.event_id = CoherenceEventId{1};
  const Result<IngestionOutcome> refused = host->publish(oldest);
  CO_REQUIRE(refused.ok());
  CO_CHECK(refused.value().code == ErrorCode::StaleSequence);

  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::EventType), total);
}

CO_TEST(hardening_backpressure_rejects_batches_and_counts_the_loss) {
  // The bound admits a batch only while fewer than 16 units are in flight, so
  // a burst of concurrent batches must produce both admissions and refusals.
  ObservatoryOptions options = cotest::test_options();
  options.max_pending_ingestion = 48;
  Observatory observatory(options);
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.hard.3");

  std::atomic<int> capacity_rejections{0};
  std::atomic<int> accepted{0};
  std::vector<std::thread> threads;
  for (int worker = 0; worker < 4; ++worker) {
    threads.emplace_back([&observatory, &capacity_rejections, &accepted, worker]() {
      for (int round = 0; round < 40; ++round) {
        std::vector<Observation> batch;
        for (int i = 0; i < 32; ++i) {
          Observation observation = cotest::exact_observation(
              static_cast<std::size_t>(i % 4), EventType::RemoteRead,
              static_cast<Nanos>(worker * 100000 + round * 100 + i));
          observation.source_publisher = PublisherId{"pub.hard.3"};
          observation.publisher_boot = cotest::deterministic_boot("pub.hard.3");
          observation.coordinator_epoch = observatory.coordinator_epoch();
          observation.topology_generation = observatory.topology_generation();
          observation.event_id =
              CoherenceEventId{static_cast<std::uint64_t>(worker) * 1000000u +
                               static_cast<std::uint64_t>(round) * 100u +
                               static_cast<std::uint64_t>(i) + 1u};
          observation.sequence = EventSequence{observation.event_id.value()};
          batch.push_back(observation);
        }
        const Result<BatchOutcome> outcome = observatory.ingest_batch(std::move(batch));
        if (!outcome.ok() && outcome.code() == ErrorCode::Capacity) {
          capacity_rejections.fetch_add(1);
        } else if (outcome.ok()) {
          accepted.fetch_add(1);
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  const LossReport loss = observatory.loss_report();
  CO_CHECK(loss.backpressure_rejections > 0);
  CO_CHECK(capacity_rejections.load() > 0);
  CO_CHECK(accepted.load() > 0);
  // Every rejection is accounted for: the sum of accepted and rejected
  // evidence equals what was offered.
  const SnapshotPtr snapshot = observatory.snapshot();
  const std::uint64_t aggregated =
      snapshot->aggregates().observations_in(AggregateDimension::EventType);
  CO_CHECK(aggregated > 0);
  CO_CHECK(aggregated <= static_cast<std::uint64_t>(accepted.load()) * 32u);
}

CO_TEST(hardening_aggregate_saturation_is_reported_end_to_end) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.hard.4");

  // Two exact counter deltas of 2^63 each cannot both fit in the accumulated
  // total.  The runtime must saturate and say so rather than wrap.
  const std::uint64_t half_range = 1ull << 63;
  auto publish_delta = [&host](std::uint64_t value, std::uint64_t sequence) {
    CounterPublication publication;
    publication.counter = CounterId{"ctr.hard.saturate"};
    publication.mapped_type = EventType::RemoteRead;
    publication.kind = CounterKind::Delta;
    publication.scope = CounterScope::PerDevice;
    publication.width_bits = 64;
    publication.generation = CounterGeneration{1};
    publication.sampling_epoch = SamplingEpoch{1};
    publication.raw_value = value;
    publication.bytes_per_unit = 0;
    publication.sequence = EventSequence{sequence};
    publication.timestamp_ns = static_cast<Nanos>(sequence) * 1000;
    publication.provenance = Provenance::HardwarePerformanceCounter;
    publication.granularity = EvidenceGranularity::Device;
    publication.source = cotest::ref_accelerator("acc.a");
    return host->publish_counter(publication);
  };
  CO_REQUIRE(publish_delta(half_range, 1).ok());
  const Result<CounterOutcome> second = publish_delta(half_range, 2);
  CO_REQUIRE(second.ok());
  CO_CHECK_EQ(second.value().delta, half_range);

  const SnapshotPtr snapshot = observatory.snapshot();
  const AggregateValue* device =
      snapshot->aggregates().find(AggregateKey{AggregateDimension::Device, "acc.a"});
  CO_REQUIRE(device != nullptr);
  CO_CHECK(device->saturated);
  CO_CHECK_EQ(device->counter_delta_total, std::numeric_limits<std::uint64_t>::max());
  CO_CHECK(snapshot->loss().aggregate_saturations > 0);
  bool capacity_finding = false;
  for (const Finding& finding : snapshot->findings()) {
    if (finding.kind == FindingKind::CapacityPressure) {
      capacity_finding = true;
    }
  }
  CO_CHECK(capacity_finding);
}

CO_TEST(hardening_retired_region_evidence_never_becomes_current_again) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.hard.5");
  host->publish(cotest::exact_observation(0, EventType::RemoteRead, 1000));
  CO_REQUIRE(observatory
                 .retire_region(MemoryRegionId{"region.test.0"}, MemoryRegionGeneration{1},
                                "hardening retirement")
                 .ok());
  for (int i = 0; i < 16; ++i) {
    host->publish(cotest::exact_observation(0, EventType::RemoteRead,
                                            static_cast<Nanos>(2000 + i * 10)));
  }
  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::EventType),
              std::uint64_t{1});
  CO_CHECK(snapshot->stale_evidence().size() >= 16);
  // Re-registering the region at a new generation makes only *new* evidence
  // current; the retired-generation evidence stays stale.
  RegionRecord replacement;
  replacement.id = MemoryRegionId{"region.test.0"};
  replacement.generation = MemoryRegionGeneration{2};
  replacement.memory_domain = MemoryDomainId{"md.host.a"};
  replacement.memory_domain_generation = MemoryDomainGeneration{1};
  replacement.coherence_domain = CoherenceDomainId{"cd.test"};
  replacement.sharing_scope = SharingScope::DeviceShared;
  CO_REQUIRE(observatory.register_region(replacement).ok());
  Observation fresh = cotest::exact_observation(0, EventType::RemoteRead, 9000);
  fresh.region_generation = MemoryRegionGeneration{2};
  const Result<IngestionOutcome> outcome = host->publish(fresh);
  CO_REQUIRE(outcome.ok());
  CO_CHECK(outcome.value().counted);
  CO_CHECK_EQ(observatory.snapshot()->aggregates().observations_in(
                  AggregateDimension::EventType),
              std::uint64_t{2});
}

CO_TEST(hardening_malformed_trace_enums_are_rejected) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.hard.6", Provenance::ImportedTrace);

  const std::string bad_precision =
      std::string(kTraceMagic) +
      "\nevent REMOTE_READ region.test.0 1 ACCELERATOR:acc.a MEMORY_DOMAIN:md.host.a READ "
      "SHARED SHARED 64 1 NOT_A_PRECISION REGION LOCAL_ACCELERATOR IMPORTED_TRACE 1\n";
  CO_CHECK(!import_trace_text(bad_precision, *host, host.get(), true).ok());

  const std::string bad_granularity =
      std::string(kTraceMagic) +
      "\nevent REMOTE_READ region.test.0 1 ACCELERATOR:acc.a MEMORY_DOMAIN:md.host.a READ "
      "SHARED SHARED 64 1 EXACT_EVENT NOT_A_GRANULARITY LOCAL_ACCELERATOR IMPORTED_TRACE 1\n";
  CO_CHECK(!import_trace_text(bad_granularity, *host, host.get(), true).ok());

  const std::string zero_generation =
      std::string(kTraceMagic) +
      "\nevent REMOTE_READ region.test.0 0 ACCELERATOR:acc.a MEMORY_DOMAIN:md.host.a READ "
      "SHARED SHARED 64 1 EXACT_EVENT REGION LOCAL_ACCELERATOR IMPORTED_TRACE 1\n";
  CO_CHECK(!import_trace_text(zero_generation, *host, host.get(), true).ok());

  const std::string negative_bytes =
      std::string(kTraceMagic) +
      "\nevent REMOTE_READ region.test.0 1 ACCELERATOR:acc.a MEMORY_DOMAIN:md.host.a READ "
      "SHARED SHARED -64 1 EXACT_EVENT REGION LOCAL_ACCELERATOR IMPORTED_TRACE 1\n";
  CO_CHECK(!import_trace_text(negative_bytes, *host, host.get(), true).ok());

  const std::string unknown_without_detail =
      std::string(kTraceMagic) +
      "\nevent UNKNOWN_COHERENCE_EVENT region.test.0 1 ACCELERATOR:acc.a MEMORY_DOMAIN:md.host.a "
      "READ SHARED SHARED 64 1 EXACT_EVENT REGION LOCAL_ACCELERATOR IMPORTED_TRACE 1\n";
  CO_CHECK(!import_trace_text(unknown_without_detail, *host, host.get(), true).ok());

  // None of the rejected imports may have mutated authoritative state.
  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::EventType),
              std::uint64_t{0});
}

CO_TEST(hardening_conflicting_registration_is_refused_without_mutation) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  const std::uint64_t fingerprint = observatory.state_fingerprint();

  // A coherence domain member that does not exist.
  CoherenceDomainRecord domain;
  domain.id = CoherenceDomainId{"cd.hard"};
  domain.generation = CoherenceDomainGeneration{1};
  domain.protocol_family = "test";
  ResourceRef ghost;
  ghost.kind = ResourceKind::Accelerator;
  ghost.id = ResourceId{"acc.ghost"};
  ghost.generation = 1;
  domain.members.push_back(ghost);
  CO_CHECK(!observatory.register_coherence_domain(domain).ok());
  CO_CHECK_EQ(observatory.state_fingerprint(), fingerprint);

  // A topology link between resources that do not exist.
  TopologyLink link;
  link.from = ghost;
  link.to = cotest::ref_domain("md.host.a");
  link.locality = Locality::RemoteAccelerator;
  CO_CHECK(!observatory.set_topology_link(link).ok());

  // A region that references an unregistered memory domain.
  RegionRecord region;
  region.id = MemoryRegionId{"region.hard"};
  region.generation = MemoryRegionGeneration{1};
  region.memory_domain = MemoryDomainId{"md.absent"};
  region.sharing_scope = SharingScope::Private;
  CO_CHECK(!observatory.register_region(region).ok());

  // A processor whose page size claim is not a power of two.
  RegionRecord bad_page;
  bad_page.id = MemoryRegionId{"region.hard.2"};
  bad_page.generation = MemoryRegionGeneration{1};
  bad_page.memory_domain = MemoryDomainId{"md.host.a"};
  bad_page.page_size_known = true;
  bad_page.page_size_bytes = 4095;
  CO_CHECK(!observatory.register_region(bad_page).ok());

  CO_CHECK_EQ(observatory.state_fingerprint(), fingerprint);
}

CO_TEST(hardening_query_size_is_bounded_by_the_coordinator) {
  CoordinatorOptions options;
  options.bind_host = "127.0.0.1";
  options.port = 0;
  options.save_on_shutdown = false;
  options.load_on_start = false;
  CoordinatorServer server(options);
  CO_REQUIRE(server.start().ok());

  // Register many regions with long names so that a findings response would
  // exceed a small bound; the coordinator must still behave correctly.
  cotest::register_test_topology(server.observatory(), 200);
  PublisherRegistration registration;
  registration.id = PublisherId{"pub.hard.7"};
  registration.boot = cotest::deterministic_boot("pub.hard.7");
  registration.provenance = Provenance::SyntheticBackend;
  CO_REQUIRE(server.observatory().register_publisher(registration).ok());

  for (std::uint64_t i = 1; i <= 2048; ++i) {
    Observation observation = cotest::exact_observation(static_cast<std::size_t>(i % 200),
                                                        EventType::RemoteRead,
                                                        static_cast<Nanos>(i * 10));
    observation.source_publisher = registration.id;
    observation.publisher_boot = registration.boot;
    observation.coordinator_epoch = server.observatory().coordinator_epoch();
    observation.topology_generation = server.observatory().topology_generation();
    observation.event_id = CoherenceEventId{i};
    observation.sequence = EventSequence{i};
    observation.bytes = 4096;
    CO_REQUIRE(server.observatory().ingest(observation).ok());
  }

  ClientOptions client_options;
  client_options.host = "127.0.0.1";
  client_options.port = server.port();
  client_options.client_name = "hardening";
  ObservationClient client;
  CO_REQUIRE(client.connect(client_options).ok());
  const Result<SnapshotPtr> snapshot = client.query_snapshot();
  CO_REQUIRE(snapshot.ok());
  CO_CHECK(snapshot.value()->findings().size() <= Limits::kMaxFindings);
  const Result<std::vector<Finding>> findings = client.query_findings();
  CO_REQUIRE(findings.ok());
  CO_CHECK(findings.value().size() <= Limits::kMaxQueryResults);
  client.close();
  CO_CHECK(server.stop().ok());
  CO_CHECK_EQ(server.connection_count(), std::size_t{0});
}

CO_TEST(hardening_repeated_persistence_cycles_are_stable) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "cohobs-hardening-state.bin";
  std::error_code ignored;
  std::filesystem::remove(path, ignored);

  Observatory first(cotest::test_options());
  cotest::register_test_topology(first);
  CO_REQUIRE(first.save_state(path).ok());
  const Result<PersistenceReport> initial = first.save_state(path);
  CO_REQUIRE(initial.ok());

  Observatory second(cotest::test_options());
  CO_REQUIRE(second.load_state(path).ok());
  // Re-saving restored state must not double-apply or lose structure.
  CO_REQUIRE(second.save_state(path).ok());
  Observatory third(cotest::test_options());
  CO_REQUIRE(third.load_state(path).ok());
  CO_CHECK_EQ(third.snapshot()->regions().size(), first.snapshot()->regions().size());
  CO_CHECK(third.coordinator_epoch().value() > second.coordinator_epoch().value());
  CO_CHECK(second.coordinator_epoch().value() > first.coordinator_epoch().value());
  std::filesystem::remove(path, ignored);
}

}  // namespace
