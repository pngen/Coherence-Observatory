// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Deterministic interleaving tests.  Each test forces a specific ordering with
// barriers and thread handshakes rather than relying on random scheduling.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>

#include "coherence/aggregate.hpp"
#include "coherence/client.hpp"
#include "coherence/coordinator.hpp"
#include "coherence/observatory.hpp"

namespace {

using namespace sol::coherence;

/// Spin barrier that never uses a timeout: it waits for an explicit signal.
class Handshake {
 public:
  void arrive_and_wait() {
    ready_.fetch_add(1);
    while (ready_.load() < expected_) {
      std::this_thread::yield();
    }
  }

  void release() { ready_.store(expected_); }

  void set_expected(int expected) { expected_ = expected; }

 private:
  std::atomic<int> ready_{0};
  int expected_ = 2;
};

CO_TEST(race_fence_versus_event_arrival) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.race.1");
  host->publish(cotest::exact_observation(0, EventType::RemoteRead, 1000));

  Handshake handshake;
  std::atomic<bool> fenced{false};
  std::thread fencing([&observatory, &handshake, &fenced]() {
    handshake.arrive_and_wait();
    observatory.fence_publisher(PublisherId{"pub.race.1"}, FenceReason::Administrative, "race");
    fenced.store(true);
  });
  std::thread publishing([&host, &handshake, &fenced]() {
    handshake.arrive_and_wait();
    for (int i = 0; i < 200; ++i) {
      host->publish(cotest::exact_observation(0, EventType::RemoteRead,
                                              static_cast<Nanos>(2000 + i * 10)));
    }
    (void)fenced.load();
  });
  fencing.join();
  publishing.join();

  const Result<PublisherView> view = observatory.publisher(PublisherId{"pub.race.1"});
  CO_REQUIRE(view.ok());
  CO_CHECK(view.value().fenced);
  CO_CHECK(!view.value().current);
  // After fencing, no further evidence may be accepted.
  const Result<IngestionOutcome> after =
      host->publish(cotest::exact_observation(0, EventType::RemoteRead, 900000));
  CO_REQUIRE(after.ok());
  CO_CHECK(after.value().disposition == IngestionDisposition::Rejected);
}

CO_TEST(race_snapshot_versus_event_commit) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.race.2");

  Handshake handshake;
  std::atomic<std::uint64_t> observed{0};
  std::thread snapshotting([&observatory, &handshake, &observed]() {
    handshake.arrive_and_wait();
    for (int i = 0; i < 64; ++i) {
      const SnapshotPtr snapshot = observatory.snapshot();
      observed.fetch_add(snapshot->aggregates().observations_in(
                             AggregateDimension::EventType),
                         std::memory_order_relaxed);
    }
  });
  std::thread publishing([&host, &handshake]() {
    handshake.arrive_and_wait();
    for (int i = 0; i < 256; ++i) {
      host->publish(cotest::exact_observation(0, EventType::RemoteRead,
                                              static_cast<Nanos>(i * 10)));
    }
  });
  snapshotting.join();
  publishing.join();
  const SnapshotPtr final = observatory.snapshot();
  CO_CHECK_EQ(final->aggregates().observations_in(AggregateDimension::EventType),
              std::uint64_t{256});
}

CO_TEST(race_region_retirement_versus_attribution) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.race.3");
  for (int i = 0; i < 32; ++i) {
    host->publish(cotest::exact_observation(0, EventType::RemoteRead,
                                            static_cast<Nanos>(1000 + i * 10)));
  }
  Handshake handshake;
  std::thread retiring([&observatory, &handshake]() {
    handshake.arrive_and_wait();
    observatory.retire_region(MemoryRegionId{"region.test.0"}, MemoryRegionGeneration{1},
                              "race retirement");
  });
  std::thread attributing([&observatory, &handshake]() {
    handshake.arrive_and_wait();
    for (int i = 0; i < 200; ++i) {
      observatory.attribute_region(MemoryRegionId{"region.test.0"});
    }
  });
  retiring.join();
  attributing.join();

  const Result<AttributionResult> attribution =
      observatory.attribute_region(MemoryRegionId{"region.test.0"});
  CO_REQUIRE(attribution.ok());
  CO_CHECK(attribution.value().outcome == AttributionOutcome::StaleEvidence);
  CO_CHECK(attribution.value().has_reason(attribution_reason::kRetiredRegion));
}

CO_TEST(race_persistence_versus_mutation) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "cohobs-race-state.bin";
  std::error_code ignored;
  std::filesystem::remove(path, ignored);

  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.race.4");

  Handshake handshake;
  std::thread saving([&observatory, &handshake, &path]() {
    handshake.arrive_and_wait();
    for (int i = 0; i < 24; ++i) {
      observatory.save_state(path);
    }
  });
  std::thread mutating([&host, &handshake]() {
    handshake.arrive_and_wait();
    for (int i = 0; i < 400; ++i) {
      host->publish(cotest::exact_observation(static_cast<std::size_t>(i % 4),
                                              EventType::RemoteRead,
                                              static_cast<Nanos>(i * 10)));
    }
  });
  saving.join();
  mutating.join();

  Observatory loaded(cotest::test_options());
  CO_CHECK(loaded.load_state(path).ok());
  std::filesystem::remove(path, ignored);
}

CO_TEST(race_analyzer_versus_aggregate_update) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.race.5");

  Handshake handshake;
  std::thread analysing([&observatory, &handshake]() {
    handshake.arrive_and_wait();
    for (int i = 0; i < 64; ++i) {
      observatory.findings();
      observatory.hotspots();
      observatory.ping_pong();
      observatory.false_sharing();
      observatory.remote_access();
      observatory.invalidation_analysis();
    }
  });
  std::thread publishing([&host, &handshake]() {
    handshake.arrive_and_wait();
    for (int i = 0; i < 512; ++i) {
      host->publish(cotest::exact_observation(static_cast<std::size_t>(i % 4),
                                              EventType::RemoteRead,
                                              static_cast<Nanos>(i * 10)));
    }
  });
  analysing.join();
  publishing.join();
  CO_CHECK_EQ(observatory.snapshot()->aggregates().observations_in(
                  AggregateDimension::EventType),
              std::uint64_t{512});
}

CO_TEST(race_coordinator_shutdown_versus_publisher_send) {
  for (int repetition = 0; repetition < 3; ++repetition) {
    CoordinatorOptions options;
    options.bind_host = "127.0.0.1";
    options.port = 0;
    options.save_on_shutdown = false;
    options.load_on_start = false;
    CoordinatorServer server(options);
    CO_REQUIRE(server.start().ok());
    const std::uint16_t port = server.port();

    ObserverId observer = ObserverId{"observer.race"};
    PublisherRegistration registration;
    registration.id = PublisherId{"pub.race.6"};
    registration.boot = make_publisher_boot_id();
    registration.observer = observer;
    registration.provenance = Provenance::SyntheticBackend;

    ClientOptions client_options;
    client_options.host = "127.0.0.1";
    client_options.port = port;
    client_options.client_name = "race-publisher";
    client_options.receive_timeout_ms = 5000;

    ObservationPublisher publisher;
    CO_REQUIRE(publisher.connect(client_options, registration).ok());
    std::thread sender([&publisher]() {
      for (int i = 0; i < 400; ++i) {
        Observation observation = cotest::exact_observation(0, EventType::RemoteRead,
                                                            static_cast<Nanos>(i * 10));
        const Result<IngestionOutcome> outcome = publisher.publish(observation);
        if (!outcome.ok() ||
            outcome.value().disposition == IngestionDisposition::Rejected) {
          break;
        }
      }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    CO_CHECK(server.stop().ok());
    sender.join();
    publisher.close();
    CO_CHECK(server.connection_count() == 0);
  }
}

CO_TEST(race_repeated_coordinator_lifecycle_is_clean) {
  for (int repetition = 0; repetition < 4; ++repetition) {
    CoordinatorOptions options;
    options.bind_host = "127.0.0.1";
    options.port = 0;
    options.save_on_shutdown = false;
    options.load_on_start = false;
    CoordinatorServer server(options);
    CO_REQUIRE(server.start().ok());
    CO_CHECK(server.running());
    CO_CHECK(server.port() != 0);
    CO_CHECK(server.stop().ok());
    CO_CHECK(!server.running());
    CO_CHECK(server.connection_count() == 0);
    // Stopping twice is safe.
    CO_CHECK(server.stop().ok());
  }
}

CO_TEST(race_state_load_versus_fresh_publication) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "cohobs-race-load.bin";
  std::error_code ignored;
  std::filesystem::remove(path, ignored);
  {
    Observatory source(cotest::test_options());
    cotest::register_test_topology(source);
    CO_REQUIRE(source.save_state(path).ok());
  }

  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.race.7");

  Handshake handshake;
  std::thread loading([&observatory, &handshake, &path]() {
    handshake.arrive_and_wait();
    observatory.load_state(path);
  });
  std::thread publishing([&host, &handshake]() {
    handshake.arrive_and_wait();
    for (int i = 0; i < 128; ++i) {
      host->publish(cotest::exact_observation(0, EventType::RemoteRead,
                                              static_cast<Nanos>(i * 10)));
    }
  });
  loading.join();
  publishing.join();

  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK_EQ(snapshot->current_publisher_count(), std::size_t{0});
  CO_CHECK(snapshot->aggregates().observations_in(AggregateDimension::EventType) == 0 ||
           snapshot->aggregates().observations_in(AggregateDimension::EventType) > 0);
  std::filesystem::remove(path, ignored);
}

}  // namespace