// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Coordinator restart in process: durable structure is restored, the
// coordinator epoch advances, publishers are not current, prior-run activity
// stays historical and never becomes current evidence.

#include <cstdio>
#include <filesystem>
#include <iostream>

#include "coherence/backends/synthetic.hpp"
#include "example_support.hpp"

int main() {
  const std::filesystem::path state_path =
      std::filesystem::temp_directory_path() / "cohobs-example-restart.bin";
  std::error_code ignored;
  std::filesystem::remove(state_path, ignored);

  sol::coherence::CoordinatorEpoch first_epoch;
  {
    sol::coherence::Observatory observatory(example::default_options());
    example::register_topology(observatory);

    std::unique_ptr<sol::coherence::LocalPublisherHost> storage;
    sol::coherence::LocalPublisherHost* host = nullptr;
    if (!example::start_host(observatory, &host, &storage).ok()) {
      std::cerr << "error: could not start publisher host\n";
      return 1;
    }
    sol::coherence::SyntheticConfig config =
        sol::coherence::make_preset_config(sol::coherence::SyntheticPreset::RemoteNumaAccess, 7);
    sol::coherence::SyntheticBackend backend(config);
    sol::coherence::CollectorContext context;
    context.sink = host;
    context.registrar = host;
    context.seed = 7;
    if (!backend.start(context).ok()) {
      std::cerr << "error: synthetic backend failed to start\n";
      return 1;
    }
    // The preset topology is already registered, so the backend only emits.
    for (int i = 0; i < 8; ++i) {
      backend.poll(context);
    }
    first_epoch = observatory.coordinator_epoch();
    const sol::coherence::Result<sol::coherence::PersistenceReport> saved =
        observatory.save_state(state_path);
    if (!saved.ok()) {
      std::cerr << "error: " << saved.describe() << "\n";
      return 1;
    }
  }

  sol::coherence::Observatory restarted(example::default_options());
  const sol::coherence::Result<sol::coherence::PersistenceReport> loaded =
      restarted.load_state(state_path);
  if (!loaded.ok()) {
    std::cerr << "error: " << loaded.describe() << "\n";
    return 1;
  }
  std::cout << "coordinator epoch " << first_epoch.value() << " -> "
            << restarted.coordinator_epoch().value() << "\n";

  const sol::coherence::SnapshotPtr snapshot = restarted.snapshot();
  std::cout << "regions restored        " << snapshot->regions().size() << "\n";
  std::cout << "current publishers      " << snapshot->current_publisher_count() << "\n";
  std::cout << "current observations    "
            << snapshot->aggregates().observations_in(sol::coherence::AggregateDimension::EventType)
            << "\n";
  std::cout << "historical observations " << snapshot->historical_observation_count()
            << " (from coordinator epoch " << snapshot->history_source_epoch().value() << ")\n";

  std::filesystem::remove(state_path, ignored);
  const bool epoch_advanced = restarted.coordinator_epoch().value() > first_epoch.value();
  const bool nothing_current = snapshot->current_publisher_count() == 0;
  return epoch_advanced && nothing_current ? 0 : 1;
}