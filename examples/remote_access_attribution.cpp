// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Remote accesses across a peer-accelerator boundary, with locality derived
// from the registered topology and a cost figure that states how it was made.

#include <iostream>

#include "example_support.hpp"

int main() {
  sol::coherence::Observatory observatory(example::default_options());
  example::register_topology(observatory);

  std::unique_ptr<sol::coherence::LocalPublisherHost> storage;
  sol::coherence::LocalPublisherHost* host = nullptr;
  if (!example::start_host(observatory, &host, &storage).ok()) {
    std::cerr << "error: could not start publisher host\n";
    return 1;
  }

  sol::coherence::Nanos clock = sol::coherence::monotonic_now_ns();
  for (int i = 0; i < 32; ++i) {
    sol::coherence::Observation observation;
    observation.type = sol::coherence::EventType::RemoteRead;
    observation.timestamp_ns = clock;
    clock += 1000;
    observation.direction = sol::coherence::AccessDirection::Read;
    observation.bytes = 4096;
    observation.pages = 1;
    observation.source = example::accelerator_ref("acc.b");
    observation.target = example::domain_ref("md.host");
    observation.region = sol::coherence::MemoryRegionId{"region.shared"};
    observation.region_generation = sol::coherence::MemoryRegionGeneration{1};
    observation.precision = sol::coherence::Precision::ExactEvent;
    observation.granularity = sol::coherence::EvidenceGranularity::Region;
    observation.provenance = sol::coherence::Provenance::SyntheticBackend;
    // Locality is deliberately NOT declared: the runtime derives it from the
    // topology generation the evidence is bound to.
    host->publish(std::move(observation));
  }

  const sol::coherence::Result<std::vector<sol::coherence::RemoteAccessRow>> rows =
      observatory.remote_access();
  if (!rows.ok()) {
    std::cerr << "error: " << rows.describe() << "\n";
    return 1;
  }
  for (const sol::coherence::RemoteAccessRow& row : rows.value()) {
    std::cout << row.source << " -> " << row.target << " region " << row.region.str()
              << " locality "
              << (row.locality_established ? sol::coherence::to_string(row.locality)
                                           : std::string_view("UNKNOWN"))
              << " accesses " << row.accesses << " bytes " << row.bytes << " cost "
              << sol::coherence::to_string(row.cost.kind) << " precision "
              << sol::coherence::to_string(row.precision) << " topology_gen "
              << row.topology_generation.value() << "\n";
  }
  return rows.value().empty() ? 1 : 0;
}