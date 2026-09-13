// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <thread>
#include <vector>

#include "coherence/aggregate.hpp"
#include "coherence/observatory.hpp"

namespace {

using namespace sol::coherence;

CO_TEST(accepted_observation_counts_once) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);

  const Result<IngestionOutcome> outcome =
      host->publish(cotest::exact_observation(0, EventType::RemoteRead, 1000));
  CO_REQUIRE(outcome.ok());
  CO_CHECK(outcome.value().disposition == IngestionDisposition::Accepted);
  CO_CHECK(outcome.value().counted);

  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::EventType),
              std::uint64_t{1});
  CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::Region),
              std::uint64_t{1});
  CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::EventType),
              snapshot->aggregates().observations_in(AggregateDimension::Device));
}

CO_TEST(duplicate_is_idempotent_and_conflict_is_rejected) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);

  Observation first = cotest::exact_observation(0, EventType::RemoteRead, 1000);
  first.event_id = CoherenceEventId{777};
  first.sequence = EventSequence{5};
  CO_CHECK(host->publish(first).value().disposition == IngestionDisposition::Accepted);

  const std::uint64_t fingerprint = observatory.state_fingerprint();

  Observation duplicate = first;
  const Result<IngestionOutcome> duplicated = host->publish(duplicate);
  CO_REQUIRE(duplicated.ok());
  CO_CHECK(duplicated.value().disposition == IngestionDisposition::Duplicate);
  CO_CHECK(!duplicated.value().counted);

  Observation conflicting = first;
  conflicting.bytes = 4096;
  const Result<IngestionOutcome> conflict = host->publish(conflicting);
  CO_REQUIRE(conflict.ok());
  CO_CHECK(conflict.value().disposition == IngestionDisposition::Rejected);
  CO_CHECK(conflict.value().code == ErrorCode::Conflict);

  // A conflicting duplicate must not have mutated authoritative state.
  CO_CHECK_EQ(observatory.state_fingerprint(), fingerprint);
  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::EventType),
              std::uint64_t{1});
  CO_CHECK_EQ(snapshot->loss().loss.rejected_duplicates, std::uint64_t{1});
  CO_CHECK_EQ(snapshot->loss().loss.rejected_conflicting_duplicates, std::uint64_t{1});
}

CO_TEST(sequence_gaps_and_late_events_are_explicit) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);

  Observation first = cotest::exact_observation(0, EventType::RemoteRead, 1000);
  first.sequence = EventSequence{1};
  host->publish(first);

  Observation skipped = cotest::exact_observation(0, EventType::RemoteRead, 2000);
  skipped.sequence = EventSequence{5};
  const Result<IngestionOutcome> gap = host->publish(skipped);
  CO_REQUIRE(gap.ok());
  CO_CHECK_EQ(gap.value().missing_sequences, std::uint64_t{3});

  Observation late = cotest::exact_observation(0, EventType::RemoteRead, 1500);
  late.sequence = EventSequence{3};
  const Result<IngestionOutcome> late_outcome = host->publish(late);
  CO_REQUIRE(late_outcome.ok());
  CO_CHECK(late_outcome.value().disposition == IngestionDisposition::AcceptedLate);

  const Result<PublisherView> view = observatory.publisher(PublisherId{"pub.test.1"});
  CO_REQUIRE(view.ok());
  CO_CHECK_EQ(view.value().sequences.missing_events, std::uint64_t{3});
  CO_CHECK_EQ(view.value().sequences.sequence_gaps, std::uint64_t{1});
  CO_CHECK_EQ(view.value().sequences.late_events, std::uint64_t{1});
  CO_CHECK_EQ(view.value().sequences.high_watermark.value(), std::uint64_t{5});
}

CO_TEST(stale_sequence_replay_is_rejected_without_mutation) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);

  // Push the watermark past the retention window so that the replayed
  // sequence is genuinely stale rather than merely late.
  const std::uint64_t total = Limits::kMaxSequenceWindow + 32;
  for (std::uint64_t i = 1; i <= total; ++i) {
    Observation observation = cotest::exact_observation(0, EventType::RemoteRead,
                                                        static_cast<Nanos>(i * 10));
    observation.sequence = EventSequence{i};
    host->publish(observation);
  }
  const std::uint64_t fingerprint = observatory.state_fingerprint();

  Observation replay = cotest::exact_observation(0, EventType::RemoteRead, 50);
  replay.sequence = EventSequence{2};
  const Result<IngestionOutcome> outcome = host->publish(replay);
  CO_REQUIRE(outcome.ok());
  CO_CHECK(outcome.value().code == ErrorCode::StaleSequence);
  CO_CHECK_EQ(observatory.state_fingerprint(), fingerprint);
  CO_CHECK_EQ(observatory.loss_report().rejected_stale_sequences, std::uint64_t{1});
}

CO_TEST(unknown_publisher_and_malformed_input_mutate_nothing) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  const std::uint64_t fingerprint = observatory.state_fingerprint();

  Observation unknown = cotest::exact_observation(0, EventType::RemoteRead, 1000);
  unknown.source_publisher = PublisherId{"pub.absent"};
  unknown.publisher_boot = cotest::deterministic_boot("pub.absent");
  unknown.event_id = CoherenceEventId{1};
  unknown.sequence = EventSequence{1};
  unknown.coordinator_epoch = observatory.coordinator_epoch();
  const Result<IngestionOutcome> outcome = observatory.ingest(unknown);
  CO_REQUIRE(outcome.ok());
  CO_CHECK(outcome.value().disposition == IngestionDisposition::Rejected);
  CO_CHECK(outcome.value().code == ErrorCode::Unauthorized);
  CO_CHECK_EQ(observatory.state_fingerprint(), fingerprint);

  auto host = cotest::start_host(observatory);
  const std::uint64_t with_publisher = observatory.state_fingerprint();

  Observation malformed = cotest::exact_observation(0, EventType::RemoteRead, 1000);
  malformed.bytes = Limits::kMaxByteCount + 1;
  const Result<IngestionOutcome> rejected = host->publish(malformed);
  CO_REQUIRE(rejected.ok());
  CO_CHECK(rejected.value().disposition == IngestionDisposition::Rejected);
  CO_CHECK(rejected.value().code == ErrorCode::OutOfRange);
  CO_CHECK_EQ(observatory.state_fingerprint(), with_publisher);
  CO_CHECK(observatory.loss_report().rejected_malformed >= 1);
}

CO_TEST(batch_ingestion_reports_per_event_outcomes) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);

  std::vector<Observation> batch;
  for (std::uint64_t i = 1; i <= 16; ++i) {
    Observation observation =
        cotest::exact_observation(static_cast<std::size_t>(i % 4), EventType::RemoteRead,
                                  static_cast<Nanos>(i * 100));
    observation.source_publisher = host->publisher_id();
    observation.publisher_boot = host->publisher_boot();
    observation.coordinator_epoch = observatory.coordinator_epoch();
    observation.topology_generation = observatory.topology_generation();
    observation.event_id = CoherenceEventId{i};
    observation.sequence = EventSequence{i};
    batch.push_back(observation);
  }
  Observation duplicate = batch[3];
  for (int i = 0; i < 3; ++i) {
    batch.push_back(duplicate);
  }

  const Result<BatchOutcome> outcome = observatory.ingest_batch(batch);
  CO_REQUIRE(outcome.ok());
  CO_CHECK_EQ(outcome.value().accepted, std::uint64_t{16});
  CO_CHECK_EQ(outcome.value().duplicates, std::uint64_t{3});
  CO_CHECK_EQ(outcome.value().rejected, std::uint64_t{0});
  CO_CHECK_EQ(outcome.value().size(), std::size_t{19});
  // The batch was submitted directly to the observatory rather than through
  // the host, so the host's own counter is not involved here.
  CO_CHECK_EQ(outcome.value().per_event[16].disposition, IngestionDisposition::Duplicate);

  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::EventType),
              std::uint64_t{16});
}

CO_TEST(oversized_batch_is_refused) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  std::vector<Observation> batch(Limits::kMaxBatchEvents + 1,
                                 cotest::exact_observation(0, EventType::RemoteRead, 1));
  const Result<BatchOutcome> outcome = observatory.ingest_batch(std::move(batch));
  CO_CHECK(!outcome.ok());
  CO_CHECK(outcome.code() == ErrorCode::TooLarge);
}

CO_TEST(concurrent_ingestion_keeps_accounting_exact) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host_a = cotest::start_host(observatory, "pub.test.a");
  auto host_b = cotest::start_host(observatory, "pub.test.b");

  constexpr int kPerThread = 500;
  std::thread first([&host_a]() {
    for (int i = 0; i < kPerThread; ++i) {
      host_a->publish(cotest::exact_observation(0, EventType::RemoteRead,
                                                static_cast<Nanos>(i * 10)));
    }
  });
  std::thread second([&host_b]() {
    for (int i = 0; i < kPerThread; ++i) {
      host_b->publish(cotest::exact_observation(1, EventType::RemoteWrite,
                                                static_cast<Nanos>(i * 10)));
    }
  });
  first.join();
  second.join();

  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::EventType),
              std::uint64_t{2 * kPerThread});
  CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::Publisher),
              std::uint64_t{2 * kPerThread});
  CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::Region),
              std::uint64_t{2 * kPerThread});
  CO_CHECK_EQ(snapshot->current_publisher_count(), std::size_t{2});
}

CO_TEST(aggregate_totals_match_accepted_observations) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  std::uint64_t accepted = 0;
  for (std::uint64_t i = 1; i <= 250; ++i) {
    Observation observation = cotest::exact_observation(static_cast<std::size_t>(i % 4),
                                                        EventType::RemoteRead,
                                                        static_cast<Nanos>(i * 100));
    observation.sequence = EventSequence{i};
    const Result<IngestionOutcome> outcome = host->publish(observation);
    if (outcome.ok() && outcome.value().counted) {
      ++accepted;
    }
  }
  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::EventType), accepted);
  CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::Locality), accepted);
  // The observation carries no workload, so it aggregates into the explicit
  // "unknown" bucket rather than being silently dropped.
  CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::Workload), accepted);
  const AggregateValue* unknown_workload = snapshot->aggregates().find(
      AggregateKey{AggregateDimension::Workload, "unknown"});
  CO_REQUIRE(unknown_workload != nullptr);
  CO_CHECK_EQ(unknown_workload->observations, accepted);
  const Result<PublisherView> view = observatory.publisher(PublisherId{"pub.test.1"});
  CO_REQUIRE(view.ok());
  CO_CHECK_EQ(view.value().accepted_events, accepted);
}

}  // namespace