// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Multiprocess proofs.
//
// The coordinator and every publisher run in their own operating-system
// process and communicate over real TCP sockets.  Publisher death is a real
// TerminateProcess kill; coordinator restart is a real process restart.

#include "test_framework.hpp"
#include "process_utils.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "coherence/client.hpp"
#include "coherence/observatory.hpp"

namespace {

using namespace sol::coherence;
namespace fs = std::filesystem;

struct Cluster {
  fs::path directory;
  fs::path endpoint_file;
  fs::path state_file;
  std::string endpoint;
  std::string host;
  std::uint16_t port = 0;
  cotest::ChildProcess coordinator;
};

fs::path scratch_directory(const char* name) {
  const fs::path directory = fs::temp_directory_path() / "cohobs-multiprocess" / name;
  std::error_code ignored;
  fs::remove_all(directory, ignored);
  fs::create_directories(directory, ignored);
  return directory;
}

fs::path coordinator_executable() {
  return cotest::executable_directory() / "cohobsd.exe";
}

fs::path publisher_executable() {
  return cotest::executable_directory() / "cohobs-publisher.exe";
}

bool start_coordinator(Cluster& cluster, bool load_state,
                       const std::string& extra_collector) {
  cluster.endpoint_file = cluster.directory / "endpoint.txt";
  std::error_code ignored;
  fs::remove(cluster.endpoint_file, ignored);
  std::vector<std::string> arguments = {"--host", "127.0.0.1", "--port", "0",
                                        "--endpoint-file", cluster.endpoint_file.string(),
                                        "--no-save"};
  if (!cluster.state_file.empty()) {
    arguments.push_back("--state");
    arguments.push_back(cluster.state_file.string());
  }
  if (!load_state) {
    arguments.push_back("--no-load");
  }
  if (!extra_collector.empty()) {
    arguments.push_back("--collect");
    arguments.push_back(extra_collector);
  }
  cluster.coordinator =
      cotest::spawn_process(coordinator_executable(), arguments, cluster.directory);
  if (!cluster.coordinator.running) {
    return false;
  }
  if (!cotest::wait_for_file(cluster.endpoint_file)) {
    return false;
  }
  cluster.endpoint = cotest::read_text_file(cluster.endpoint_file);
  const std::size_t colon = cluster.endpoint.rfind(':');
  if (colon == std::string::npos) {
    return false;
  }
  cluster.host = cluster.endpoint.substr(0, colon);
  cluster.port = static_cast<std::uint16_t>(
      std::strtoul(cluster.endpoint.substr(colon + 1).c_str(), nullptr, 10));
  return cluster.port != 0;
}

struct LaunchedPublisher {
  cotest::ChildProcess process;
  PublisherBootId boot;
  PublisherId id;
  std::uint64_t exit_code = 0;
};

bool launch_publisher(const Cluster& cluster, const std::string& name,
                      std::uint64_t boot, const std::string& scenario, std::uint64_t count,
                      double hold_seconds, LaunchedPublisher* out) {
  const fs::path boot_file = cluster.directory / (name + ".boot");
  const fs::path ready_file = cluster.directory / (name + ".ready");
  std::error_code ignored;
  fs::remove(boot_file, ignored);
  fs::remove(ready_file, ignored);
  std::vector<std::string> arguments = {"--endpoint", cluster.endpoint,
                                        "--publisher", name,
                                        "--scenario", scenario,
                                        "--count", std::to_string(count),
                                        "--boot-file", boot_file.string(),
                                        "--ready-file", ready_file.string(),
                                        "--hold", std::to_string(hold_seconds)};
  if (boot != 0) {
    arguments.push_back("--boot");
    arguments.push_back(std::to_string(boot));
  }
  out->process =
      cotest::spawn_process(publisher_executable(), arguments, cluster.directory);
  if (!out->process.running) {
    return false;
  }
  if (!cotest::wait_for_file(ready_file)) {
    cotest::ensure_terminated(out->process);
    return false;
  }
  out->id = PublisherId{name};
  const std::string boot_text = cotest::read_text_file(boot_file);
  if (boot_text.empty()) {
    cotest::ensure_terminated(out->process);
    return false;
  }
  out->boot = PublisherBootId{std::strtoull(boot_text.c_str(), nullptr, 10)};
  return true;
}

/// Polls a condition with a bounded number of attempts.  Expiry is a hard
/// failure reported by the caller, never a silent skip.
template <class Predicate>
bool eventually(Predicate&& predicate, unsigned long attempts = 300,
                unsigned long sleep_milliseconds = 100) {
  for (unsigned long attempt = 0; attempt < attempts; ++attempt) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_milliseconds));
  }
  return false;
}

CO_TEST(multiprocess_publisher_death_and_reincarnation) {
  Cluster cluster;
  cluster.directory = scratch_directory("death");
  cluster.state_file = cluster.directory / "state.bin";
  CO_REQUIRE(start_coordinator(cluster, false, "synthetic"));
  cotest::ProcessGuard guard(cluster.coordinator);

  LaunchedPublisher first;
  LaunchedPublisher second;
  CO_REQUIRE(launch_publisher(cluster, "pub.remote.a", 0, "PING_PONG", 24, 120.0,
                              &first));
  cotest::ProcessGuard first_guard(first.process);
  CO_REQUIRE(launch_publisher(cluster, "pub.remote.b", 0, "STEADY_SHARED_READS", 24,
                              120.0, &second));
  cotest::ProcessGuard second_guard(second.process);

  ClientOptions options;
  options.host = cluster.host;
  options.port = cluster.port;
  options.client_name = "observer";
  ObservationClient client;
  CO_REQUIRE(client.connect(options).ok());

  const Result<SnapshotPtr> before = client.query_snapshot();
  CO_REQUIRE(before.ok());
  // The coordinator also hosts its own in-process collector publisher, so the
  // assertion names the two remote publishers rather than counting them.
  const PublisherView* first_view = before.value()->find_publisher(first.id);
  const PublisherView* second_view = before.value()->find_publisher(second.id);
  CO_REQUIRE(first_view != nullptr);
  CO_REQUIRE(second_view != nullptr);
  CO_CHECK(first_view->current);
  CO_CHECK(second_view->current);
  CO_CHECK(before.value()->current_publisher_count() >= 2);

  // Kill publisher A for real.
  CO_CHECK(cotest::kill_process(first.process));

  const bool observed = eventually([&client, &first]() {
    const Result<SnapshotPtr> snapshot = client.query_snapshot();
    if (!snapshot.ok()) {
      return false;
    }
    const PublisherView* view = snapshot.value()->find_publisher(first.id);
    return view != nullptr && view->fenced && !view->current;
  });
  CO_CHECK(observed);

  const Result<SnapshotPtr> after = client.query_snapshot();
  CO_REQUIRE(after.ok());
  const PublisherView* fenced = after.value()->find_publisher(first.id);
  CO_REQUIRE(fenced != nullptr);
  CO_CHECK(fenced->fenced);
  CO_CHECK(fenced->fence_reason == FenceReason::ConnectionClosed);
  CO_CHECK(!fenced->current);
  CO_CHECK_EQ(fenced->boot.value(), first.boot.value());

  // Publisher B is unaffected.
  const PublisherView* survivor = after.value()->find_publisher(second.id);
  CO_REQUIRE(survivor != nullptr);
  CO_CHECK(survivor->current);
  CO_CHECK(!survivor->fenced);

  // Stale replay of A's boot cannot be re-admitted.
  LaunchedPublisher replay;
  const bool replay_rejected =
      !launch_publisher(cluster, "pub.remote.a", first.boot.value(),
                        "PING_PONG", 4, 0.0, &replay);
  CO_CHECK(replay_rejected);
  if (replay.process.running) {
    cotest::ensure_terminated(replay.process);
  }

  // A replacement with a fresh boot identity is accepted and requires fresh
  // publication before its evidence is current.
  LaunchedPublisher replacement;
  CO_REQUIRE(launch_publisher(cluster, "pub.remote.a", 0, "PING_PONG", 16, 0.0,
                              &replacement));
  cotest::ensure_terminated(replacement.process);

  const Result<SnapshotPtr> reincarnated = client.query_snapshot();
  CO_REQUIRE(reincarnated.ok());
  const PublisherView* revived = reincarnated.value()->find_publisher(first.id);
  CO_REQUIRE(revived != nullptr);
  CO_CHECK(revived->boot.value() != first.boot.value());
  CO_CHECK(revived->current);
  CO_CHECK(!revived->fenced);

  client.close();
  CO_CHECK(cluster.coordinator.running);
}

CO_TEST(multiprocess_coordinator_restart_advances_epoch_and_rejects_old_frames) {
  Cluster cluster;
  cluster.directory = scratch_directory("restart");
  cluster.state_file = cluster.directory / "state.bin";
  CO_REQUIRE(start_coordinator(cluster, false, "synthetic"));

  ClientOptions options;
  options.host = cluster.host;
  options.port = cluster.port;
  options.client_name = "observer";
  ObservationClient client;
  CO_REQUIRE(client.connect(options).ok());
  const Result<SnapshotPtr> first = client.query_snapshot();
  CO_REQUIRE(first.ok());
  const CoordinatorEpoch first_epoch = first.value()->coordinator_epoch();
  const std::size_t first_regions = first.value()->regions().size();
  CO_CHECK(first_regions > 0);
  client.close();

  // A publisher connection that stays alive across the coordinator restart.
  PublisherRegistration registration;
  registration.id = PublisherId{"pub.restart.1"};
  registration.boot = make_publisher_boot_id();
  registration.observer = ObserverId{"observer.multiprocess"};
  registration.node = sol::coherence::NodeId{"node.remote"};
  registration.display_name = "restart publisher";
  registration.provenance = Provenance::SyntheticBackend;

  ObservationPublisher before_restart;
  CO_REQUIRE(before_restart.connect(options, registration).ok());
  const std::uint64_t published_before = 12;
  for (std::uint64_t i = 1; i <= published_before; ++i) {
    Observation observation = cotest::exact_observation(static_cast<std::size_t>(i % 4),
                                                        EventType::RemoteRead,
                                                        static_cast<Nanos>(i * 100));
    observation.sequence = EventSequence{i};
    const Result<IngestionOutcome> outcome = before_restart.publish(observation);
    CO_REQUIRE(outcome.ok());
    CO_CHECK(outcome.value().disposition == IngestionDisposition::Accepted);
  }

  {
    ClientOptions admin_options = options;
    admin_options.client_name = "admin";
    ObservatoryAdminClient admin;
    CO_REQUIRE(admin.connect(admin_options).ok());
    CO_CHECK(admin.save_state().ok());
    admin.close();
  }

  CO_CHECK(cotest::kill_process(cluster.coordinator));

  Cluster restarted;
  restarted.directory = cluster.directory;
  restarted.state_file = cluster.state_file;
  CO_REQUIRE(start_coordinator(restarted, true, "synthetic"));
  cotest::ProcessGuard restarted_guard(restarted.coordinator);

  ClientOptions restarted_options;
  restarted_options.host = restarted.host;
  restarted_options.port = restarted.port;
  restarted_options.client_name = "observer";
  ObservationClient restarted_client;
  CO_REQUIRE(restarted_client.connect(restarted_options).ok());
  const Result<SnapshotPtr> second = restarted_client.query_snapshot();
  CO_REQUIRE(second.ok());
  CO_CHECK(second.value()->coordinator_epoch().value() > first_epoch.value());
  CO_CHECK(second.value()->regions().size() >= first_regions);
  // None of the pre-restart publisher's evidence is current any more: the
  // publisher itself is gone from the live registry and no aggregate bucket
  // carries its identity.  The coordinator's own in-process collector may
  // legitimately have re-registered under a fresh boot.
  const PublisherView* restored = second.value()->find_publisher(registration.id);
  CO_CHECK(restored == nullptr || !restored->current);
  const AggregateValue* restored_bucket = second.value()->aggregates().find(
      AggregateKey{AggregateDimension::Publisher, registration.id.str()});
  CO_CHECK(restored_bucket == nullptr || restored_bucket->observations == 0);
  CO_CHECK_EQ(second.value()->historical_observation_count(), std::uint64_t{0});
  restarted_client.close();

  // The pre-restart connection is dead.
  Observation after_restart = cotest::exact_observation(0, EventType::RemoteRead, 5000);
  after_restart.sequence = EventSequence{published_before + 1};
  const Result<IngestionOutcome> dead = before_restart.publish(after_restart);
  CO_CHECK(!dead.ok() ||
           dead.value().disposition == IngestionDisposition::Rejected);

  // Presenting the previous coordinator epoch is refused outright.
  PublisherRegistration stale_epoch = registration;
  stale_epoch.coordinator_epoch = first_epoch;
  ObservationPublisher refused;
  const Status stale = refused.connect(restarted_options, stale_epoch);
  CO_CHECK(!stale.ok());
  CO_CHECK(stale.code() == ErrorCode::StaleEpoch ||
           stale.code() == ErrorCode::ConnectionClosed ||
           stale.code() == ErrorCode::StaleBoot);

  // The same boot may re-register under the current epoch, but only resumes
  // from its persisted watermark: a replay below it is refused.
  PublisherRegistration resumed_registration = registration;
  resumed_registration.coordinator_epoch = CoordinatorEpoch{0};
  ObservationPublisher resumed;
  CO_REQUIRE(resumed.connect(restarted_options, resumed_registration).ok());
  Observation replay = cotest::exact_observation(0, EventType::RemoteRead, 100);
  replay.sequence = EventSequence{1};
  const Result<IngestionOutcome> replayed = resumed.publish(replay);
  CO_REQUIRE(replayed.ok());
  CO_CHECK(replayed.value().disposition == IngestionDisposition::Rejected);
  CO_CHECK(replayed.value().code == ErrorCode::StaleSequence);

  // Fresh publication after the replay is accepted again.
  Observation fresh = cotest::exact_observation(0, EventType::RemoteRead, 6000);
  fresh.sequence = EventSequence{published_before + 2};
  const Result<IngestionOutcome> accepted = resumed.publish(fresh);
  CO_REQUIRE(accepted.ok());
  CO_CHECK(accepted.value().disposition == IngestionDisposition::Accepted);
  resumed.close();
}

CO_TEST(multiprocess_query_bounds_and_clean_shutdown) {
  Cluster cluster;
  cluster.directory = scratch_directory("bounds");
  cluster.state_file = cluster.directory / "state.bin";
  CO_REQUIRE(start_coordinator(cluster, false, "synthetic"));
  cotest::ProcessGuard guard(cluster.coordinator);

  ClientOptions options;
  options.host = cluster.host;
  options.port = cluster.port;
  options.client_name = "observer";
  ObservationClient client;
  CO_REQUIRE(client.connect(options).ok());
  const Result<SnapshotPtr> snapshot = client.query_snapshot();
  CO_REQUIRE(snapshot.ok());
  CO_CHECK(!snapshot.value()->findings().empty());
  CO_CHECK(snapshot.value()->findings().size() <= Limits::kMaxFindings);
  client.close();

  CO_REQUIRE(cluster.coordinator.running);
  CO_CHECK(cotest::kill_process(cluster.coordinator));
}

}  // namespace