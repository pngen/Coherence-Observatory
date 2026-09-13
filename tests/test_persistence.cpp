// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "coherence/observatory.hpp"
#include "coherence/persistence.hpp"

namespace {

using namespace sol::coherence;

namespace fs = std::filesystem;

fs::path scratch(const std::string& name) {
  const fs::path directory = fs::temp_directory_path() / "cohobs-tests";
  std::error_code ignored;
  fs::create_directories(directory, ignored);
  const fs::path path = directory / name;
  fs::remove(path, ignored);
  return path;
}

std::vector<std::uint8_t> read_all(const fs::path& path) {
  std::ifstream stream(path, std::ios::binary);
  return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(stream),
                                   std::istreambuf_iterator<char>());
}

void write_all(const fs::path& path, const std::vector<std::uint8_t>& data) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(data.data()),
               static_cast<std::streamsize>(data.size()));
}

void populate(Observatory& observatory, const fs::path& path,
              std::uint64_t* fingerprint_out) {
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  for (std::uint64_t i = 1; i <= 12; ++i) {
    Observation observation = cotest::exact_observation(static_cast<std::size_t>(i % 4),
                                                        EventType::RemoteRead,
                                                        static_cast<Nanos>(i * 100));
    observation.sequence = EventSequence{i};
    host->publish(std::move(observation));
  }
  observatory.save_state(path);
  if (fingerprint_out != nullptr) {
    *fingerprint_out = observatory.state_fingerprint();
  }
}

CO_TEST(save_and_load_round_trip) {
  const fs::path path = scratch("roundtrip.bin");
  Observatory source(cotest::test_options());
  cotest::register_test_topology(source);
  auto host = cotest::start_host(source);
  host->publish(cotest::exact_observation(0, EventType::RemoteRead, 1000));

  // Fencing records a durable replay watermark for the boot.
  CO_REQUIRE(source
                 .fence_publisher(PublisherId{"pub.test.1"}, FenceReason::Administrative,
                                  "test fence")
                 .ok());
  const Result<PersistenceReport> saved = source.save_state(path);
  CO_REQUIRE(saved.ok());
  CO_CHECK(saved.value().bytes_written > 0);
  CO_CHECK_EQ(saved.value().regions, std::size_t{4});
  CO_CHECK(saved.value().publisher_watermarks >= 1);

  Observatory loaded(cotest::test_options());
  const Result<PersistenceReport> report = loaded.load_state(path);
  CO_REQUIRE(report.ok());
  CO_CHECK_EQ(report.value().regions, std::size_t{4});
  CO_CHECK(report.value().next_coordinator_epoch.value() > saved.value().source_coordinator_epoch.value());
  const SnapshotPtr snapshot = loaded.snapshot();
  CO_CHECK_EQ(snapshot->regions().size(), std::size_t{4});
  CO_CHECK_EQ(snapshot->coherence_domains().size(), std::size_t{1});
  CO_CHECK_EQ(snapshot->processors().size(), std::size_t{2});
  CO_CHECK_EQ(snapshot->accelerators().size(), std::size_t{2});
  CO_CHECK_EQ(snapshot->current_publisher_count(), std::size_t{0});
  fs::remove(path);
}

CO_TEST(load_advances_epoch_and_keeps_history_historical) {
  const fs::path path = scratch("history.bin");
  Observatory source(cotest::test_options());
  cotest::register_test_topology(source);
  auto host = cotest::start_host(source);
  for (std::uint64_t i = 1; i <= 8; ++i) {
    Observation observation = cotest::exact_observation(0, EventType::RemoteRead,
                                                        static_cast<Nanos>(i * 100));
    observation.sequence = EventSequence{i};
    host->publish(std::move(observation));
  }
  const CoordinatorEpoch before = source.coordinator_epoch();
  CO_REQUIRE(source.save_state(path).ok());

  Observatory loaded(cotest::test_options());
  CO_REQUIRE(loaded.load_state(path).ok());
  CO_CHECK(loaded.coordinator_epoch().value() > before.value());
  const SnapshotPtr snapshot = loaded.snapshot();
  CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::EventType),
              std::uint64_t{0});
  CO_CHECK_EQ(snapshot->current_publisher_count(), std::size_t{0});
  CO_CHECK_EQ(snapshot->historical_observation_count(), std::uint64_t{0});
  fs::remove(path);
}

CO_TEST(replay_watermarks_survive_a_restart) {
  const fs::path path = scratch("watermark.bin");
  Observatory source(cotest::test_options());
  cotest::register_test_topology(source);
  auto host = cotest::start_host(source, "pub.watermark.1");
  const PublisherBootId boot = host->publisher_boot();
  for (std::uint64_t i = 1; i <= 10; ++i) {
    Observation observation = cotest::exact_observation(0, EventType::RemoteRead,
                                                        static_cast<Nanos>(i * 100));
    observation.sequence = EventSequence{i};
    host->publish(std::move(observation));
  }
  CO_REQUIRE(source.save_state(path).ok());

  Observatory loaded(cotest::test_options());
  CO_REQUIRE(loaded.load_state(path).ok());

  PublisherRegistration reconnected;
  reconnected.id = PublisherId{"pub.watermark.1"};
  reconnected.boot = boot;
  const Result<PublisherView> view = loaded.register_publisher(reconnected);
  CO_REQUIRE(view.ok());
  CO_CHECK_EQ(view.value().sequences.high_watermark.value(), std::uint64_t{10});

  auto host_again = std::make_unique<LocalPublisherHost>(
      loaded, reconnected, LocalPublisherHost::Options{});
  // Replaying sequence 5 after the restart must be refused.
  Observation replay = cotest::exact_observation(0, EventType::RemoteRead, 500);
  replay.event_id = CoherenceEventId{5};
  replay.sequence = EventSequence{5};
  replay.source_publisher = PublisherId{"pub.watermark.1"};
  replay.publisher_boot = boot;
  replay.coordinator_epoch = loaded.coordinator_epoch();
  const Result<IngestionOutcome> outcome = loaded.ingest(replay);
  CO_REQUIRE(outcome.ok());
  CO_CHECK(outcome.value().disposition == IngestionDisposition::Rejected);
  CO_CHECK(outcome.value().code == ErrorCode::StaleSequence);
  fs::remove(path);
}

CO_TEST(old_epoch_frames_are_rejected_after_restart) {
  const fs::path path = scratch("epoch.bin");
  Observatory source(cotest::test_options());
  cotest::register_test_topology(source);
  auto host = cotest::start_host(source);
  host->publish(cotest::exact_observation(0, EventType::RemoteRead, 1000));
  const CoordinatorEpoch old_epoch = source.coordinator_epoch();
  CO_REQUIRE(source.save_state(path).ok());

  Observatory loaded(cotest::test_options());
  CO_REQUIRE(loaded.load_state(path).ok());
  Observation observation = cotest::exact_observation(0, EventType::RemoteRead, 2000);
  observation.event_id = CoherenceEventId{1};
  observation.sequence = EventSequence{1};
  observation.source_publisher = PublisherId{"pub.test.1"};
  observation.publisher_boot = host->publisher_boot();
  observation.coordinator_epoch = old_epoch;
  const Result<IngestionOutcome> outcome = loaded.ingest(observation);
  CO_REQUIRE(outcome.ok());
  CO_CHECK(outcome.value().code == ErrorCode::Unauthorized ||
           outcome.value().code == ErrorCode::StaleEpoch);
  fs::remove(path);
}

CO_TEST(empty_and_short_files_are_rejected) {
  const fs::path path = scratch("short.bin");
  Observatory observatory(cotest::test_options());
  const std::uint64_t fingerprint = observatory.state_fingerprint();

  {
    std::ofstream empty(path, std::ios::binary | std::ios::trunc);
  }
  const Result<PersistenceReport> empty_result = observatory.load_state(path);
  CO_CHECK(!empty_result.ok());
  CO_CHECK(empty_result.code() == ErrorCode::EmptyState);

  const std::vector<std::uint8_t> short_magic = {'C', 'O', 'B', 'S'};
  write_all(path, short_magic);
  const Result<PersistenceReport> short_result = observatory.load_state(path);
  CO_CHECK(!short_result.ok());
  CO_CHECK_EQ(observatory.state_fingerprint(), fingerprint);
  fs::remove(path);
}

CO_TEST(corrupt_header_and_payload_are_rejected_without_mutation) {
  const fs::path path = scratch("corrupt.bin");
  std::uint64_t fingerprint = 0;
  {
    Observatory source(cotest::test_options());
    populate(source, path, &fingerprint);
  }
  const std::vector<std::uint8_t> original = read_all(path);
  CO_REQUIRE(original.size() > 96);

  auto attempt = [&path, &original](std::size_t offset, std::uint8_t value,
                                    ErrorCode expected) {
    std::vector<std::uint8_t> data = original;
    data[offset] ^= value;
    write_all(path, data);
    Observatory observatory(cotest::test_options());
    const std::uint64_t before = observatory.state_fingerprint();
    const Result<PersistenceReport> result = observatory.load_state(path);
    CO_CHECK(!result.ok());
    if (!result.ok()) {
      CO_CHECK(result.code() == expected);
    }
    CO_CHECK_EQ(observatory.state_fingerprint(), before);
  };

  attempt(0, 0xFF, ErrorCode::InvalidMagic);            // magic
  attempt(8, 0x01, ErrorCode::UnsupportedStateVersion); // version
  // Any header mutation invalidates the header checksum first.
  attempt(16, 0x40, ErrorCode::IntegrityFailure);       // payload length
  attempt(48, 0x01, ErrorCode::IntegrityFailure);       // header checksum
  attempt(80, 0x01, ErrorCode::IntegrityFailure);       // payload body

  {
    std::vector<std::uint8_t> truncated = original;
    truncated.resize(truncated.size() / 2);
    write_all(path, truncated);
    Observatory observatory(cotest::test_options());
    const std::uint64_t before = observatory.state_fingerprint();
    const Result<PersistenceReport> result = observatory.load_state(path);
    CO_CHECK(!result.ok());
    CO_CHECK_EQ(observatory.state_fingerprint(), before);
  }
  fs::remove(path);
}

CO_TEST(oversized_and_trailing_files_are_rejected) {
  const fs::path path = scratch("oversized.bin");
  {
    Observatory source(cotest::test_options());
    populate(source, path, nullptr);
  }
  std::vector<std::uint8_t> data = read_all(path);
  data.push_back(0x00);
  write_all(path, data);
  Observatory observatory(cotest::test_options());
  const std::uint64_t before = observatory.state_fingerprint();
  const Result<PersistenceReport> result = observatory.load_state(path);
  CO_CHECK(!result.ok());
  CO_CHECK_EQ(observatory.state_fingerprint(), before);
  fs::remove(path);
}

CO_TEST(broken_region_parent_relationship_is_rejected) {
  const fs::path path = scratch("parent.bin");
  {
    Observatory source(cotest::test_options());
    populate(source, path, nullptr);
  }
  std::vector<std::uint8_t> data = read_all(path);
  // Flip a byte in the middle of the payload: whatever it hits, the section
  // checksum must reject the file.
  data[data.size() / 2] ^= 0x7F;
  write_all(path, data);
  Observatory observatory(cotest::test_options());
  const Result<PersistenceReport> result = observatory.load_state(path);
  CO_CHECK(!result.ok());
  fs::remove(path);
}

CO_TEST(interrupted_temporary_file_is_ignored) {
  const fs::path path = scratch("atomic.bin");
  {
    Observatory source(cotest::test_options());
    populate(source, path, nullptr);
  }
  const fs::path temporary = fs::path(path.string() + ".tmp");
  {
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    stream << "interrupted";
  }
  Observatory observatory(cotest::test_options());
  CO_CHECK(observatory.load_state(path).ok());
  std::error_code ignored;
  fs::remove(temporary, ignored);
  fs::remove(path, ignored);
}

CO_TEST(missing_state_file_is_not_a_corruption) {
  const fs::path path = scratch("absent.bin");
  Observatory observatory(cotest::test_options());
  const Result<PersistenceReport> result = observatory.load_state(path);
  CO_CHECK(!result.ok());
  CO_CHECK(result.code() == ErrorCode::NotFound);
}

CO_TEST(state_fingerprint_is_stable_across_save_and_load) {
  const fs::path path = scratch("fingerprint.bin");
  Observatory source(cotest::test_options());
  cotest::register_test_topology(source);
  CO_REQUIRE(source.save_state(path).ok());
  const std::vector<std::uint8_t> first = read_all(path);
  CO_REQUIRE(source.save_state(path).ok());
  const std::vector<std::uint8_t> second = read_all(path);
  CO_CHECK(first == second);
  fs::remove(path);
}

}  // namespace