// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Seeded randomized property testing.  Every iteration prints its seed on
// failure so that a reproduction is possible.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <string>
#include <vector>

#include "coherence/aggregate.hpp"
#include "coherence/observatory.hpp"

namespace {

using namespace sol::coherence;

/// Deterministic xorshift64* generator: reproducible across platforms.
class Generator {
 public:
  explicit Generator(std::uint64_t seed) : state_(seed == 0 ? 0x9E3779B97F4A7C15ull : seed) {}

  std::uint64_t next() {
    state_ ^= state_ >> 12;
    state_ ^= state_ << 25;
    state_ ^= state_ >> 27;
    return state_ * 0x2545F4914F6CDD1Dull;
  }

  std::uint64_t bounded(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }

  bool chance(std::uint32_t percent) { return bounded(100) < percent; }

 private:
  std::uint64_t state_;
};

struct Outcome {
  std::uint64_t accepted = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t rejected = 0;
  std::uint64_t missing = 0;
  std::uint64_t fingerprint = 0;
  std::uint64_t aggregated = 0;
};

Outcome run_sequence(std::uint64_t seed) {
  Generator generator(seed);
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.property.1");

  Outcome outcome;
  std::vector<Observation> published;
  Nanos clock = 1000;
  for (int step = 0; step < 220; ++step) {
    const std::uint32_t action = static_cast<std::uint32_t>(generator.bounded(100));
    if (action < 60) {
      Observation observation = cotest::exact_observation(
          static_cast<std::size_t>(generator.bounded(4)),
          static_cast<EventType>(generator.bounded(kEventTypeCount - 1)), clock);
      clock += 1 + static_cast<Nanos>(generator.bounded(50));
      observation.bytes = 64 * (1 + generator.bounded(8));
      observation.sequence = host->next_sequence();
      if (generator.chance(10)) {
        observation.coordinator_epoch = CoordinatorEpoch{generator.bounded(3) + 1};
      }
      const Result<IngestionOutcome> result = host->publish(observation);
      if (result.ok() && result.value().disposition != IngestionDisposition::Rejected) {
        published.push_back(observation);
        ++outcome.accepted;
      } else {
        ++outcome.rejected;
      }
    } else if (action < 72 && !published.empty()) {
      const Observation& replay = published[generator.bounded(published.size())];
      const Result<IngestionOutcome> result = host->publish(replay);
      if (result.ok() && result.value().disposition == IngestionDisposition::Duplicate) {
        ++outcome.duplicates;
      }
    } else if (action < 80) {
      // Skip sequences to create gaps.
      const std::uint64_t skips = 1 + generator.bounded(4);
      for (std::uint64_t i = 0; i < skips; ++i) {
        host->next_sequence();
      }
      outcome.missing += skips;
    } else if (action < 86) {
      // Adversarial input: must never mutate authoritative state.
      Observation malformed = cotest::exact_observation(0, EventType::RemoteRead, clock);
      malformed.bytes = Limits::kMaxByteCount + 1;
      host->publish(malformed);
      ++outcome.rejected;
    } else if (action < 90) {
      observatory.snapshot();
    } else if (action < 94) {
      observatory.findings();
    } else if (action < 96) {
      observatory.reconcile_current_evidence();
    } else if (action < 98) {
      observatory.attribute_region(MemoryRegionId{"region.test.0"});
    } else {
      const Result<std::vector<Finding>> findings = observatory.ping_pong();
      (void)findings;
    }
  }

  const SnapshotPtr snapshot = observatory.snapshot();
  outcome.aggregated =
      snapshot->aggregates().observations_in(AggregateDimension::EventType);
  outcome.fingerprint = observatory.state_fingerprint();
  return outcome;
}

CO_TEST(property_seeded_sequences_hold_invariants) {
  std::size_t reported = 0;
  for (std::uint64_t seed = 1; seed <= 24; ++seed) {
    const Outcome outcome = run_sequence(seed);
    if (outcome.aggregated != outcome.accepted) {
      if (reported++ < 3) {
        std::printf("  reproduction seed: %llu\n",
                    static_cast<unsigned long long>(seed));
      }
      CO_CHECK_EQ(outcome.aggregated, outcome.accepted);
    }
    if (run_sequence(seed).fingerprint != outcome.fingerprint) {
      std::printf("  nondeterministic seed: %llu\n", static_cast<unsigned long long>(seed));
      CO_CHECK(false);
    }
  }
}

CO_TEST(property_stale_publisher_never_becomes_current) {
  for (std::uint64_t seed = 100; seed <= 110; ++seed) {
    Generator generator(seed);
    Observatory observatory(cotest::test_options());
    cotest::register_test_topology(observatory);
    auto host = cotest::start_host(observatory, "pub.property.2");
    const PublisherBootId boot = host->publisher_boot();
    for (int i = 0; i < 5; ++i) {
      host->publish(cotest::exact_observation(0, EventType::RemoteRead,
                                              static_cast<Nanos>(1000 + i)));
    }
    observatory.fence_publisher_boot(PublisherId{"pub.property.2"}, boot,
                                     static_cast<FenceReason>(generator.bounded(5) + 1),
                                     "property test fence");
    // Re-registration with the same boot must always fail.
    PublisherRegistration replay;
    replay.id = PublisherId{"pub.property.2"};
    replay.boot = boot;
    CO_CHECK(!observatory.register_publisher(replay).ok());
    const Result<PublisherView> view = observatory.publisher(PublisherId{"pub.property.2"});
    CO_REQUIRE(view.ok());
    CO_CHECK(!view.value().current);
    CO_CHECK(view.value().fenced);
    for (int i = 0; i < 3; ++i) {
      const Result<IngestionOutcome> outcome =
          host->publish(cotest::exact_observation(0, EventType::RemoteRead, 5000));
      CO_REQUIRE(outcome.ok());
      CO_CHECK(outcome.value().disposition == IngestionDisposition::Rejected);
    }
  }
}

CO_TEST(property_counter_resets_never_produce_negative_traffic) {
  for (std::uint64_t seed = 200; seed <= 210; ++seed) {
    Generator generator(seed);
    Observatory observatory(cotest::test_options());
    cotest::register_test_topology(observatory);
    auto host = cotest::start_host(observatory, "pub.property.3");

    std::uint64_t raw = generator.bounded(1000);
    std::uint64_t total_bytes = 0;
    for (int sample = 0; sample < 40; ++sample) {
      CounterPublication publication;
      publication.counter = CounterId{"ctr.property"};
      publication.mapped_type = EventType::RemoteRead;
      publication.kind = generator.chance(50) ? CounterKind::Absolute
                                              : CounterKind::Resettable;
      publication.scope = CounterScope::PerDevice;
      publication.width_bits = 64;
      publication.generation = CounterGeneration{1};
      publication.sampling_epoch = SamplingEpoch{1};
      publication.bytes_per_unit = 64;
      publication.provenance = Provenance::HardwarePerformanceCounter;
      publication.granularity = EvidenceGranularity::Device;
      publication.source = cotest::ref_accelerator("acc.a");
      publication.timestamp_ns = static_cast<Nanos>(sample) * 1000;

      if (generator.chance(20)) {
        raw = generator.bounded(100);  // reset
      } else {
        raw += 1 + generator.bounded(500);
      }
      publication.raw_value = raw;
      const Result<CounterOutcome> outcome = host->publish_counter(publication);
      CO_REQUIRE(outcome.ok());
      CO_CHECK(!(outcome.value().delta > 1000000));
      total_bytes += outcome.value().delta * 64;
    }
    const SnapshotPtr snapshot = observatory.snapshot();
    const AggregateValue* device =
        snapshot->aggregates().find(AggregateKey{AggregateDimension::Device, "acc.a"});
    CO_REQUIRE(device != nullptr);
    CO_CHECK_EQ(device->bytes, total_bytes);
  }
}

CO_TEST(property_duplicate_never_double_counts) {
  for (std::uint64_t seed = 300; seed <= 310; ++seed) {
    Generator generator(seed);
    Observatory observatory(cotest::test_options());
    cotest::register_test_topology(observatory);
    auto host = cotest::start_host(observatory, "pub.property.4");
    std::vector<Observation> accepted;
    for (int i = 0; i < 30; ++i) {
      Observation observation = cotest::exact_observation(
          static_cast<std::size_t>(generator.bounded(4)), EventType::RemoteRead,
          static_cast<Nanos>(1000 + i));
      observation.sequence = host->next_sequence();
      const Result<IngestionOutcome> outcome = host->publish(observation);
      if (outcome.ok() && outcome.value().counted) {
        accepted.push_back(observation);
      }
    }
    const std::uint64_t expected = accepted.size();
    for (int i = 0; i < 20; ++i) {
      host->publish(accepted[generator.bounded(accepted.size())]);
    }
    const SnapshotPtr snapshot = observatory.snapshot();
    CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::EventType),
                expected);
  }
}

}  // namespace