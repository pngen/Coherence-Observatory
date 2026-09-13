// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Alternating ownership between two accelerators over one region, detected as
// a ping-pong pattern with participants, direction sequence and cost.

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
  const char* participants[2] = {"acc.a", "acc.b"};
  for (int i = 0; i < 12; ++i) {
    sol::coherence::Observation observation;
    observation.type = sol::coherence::EventType::OwnershipTransfer;
    observation.timestamp_ns = clock;
    clock += 20000;
    observation.source = example::accelerator_ref(participants[i % 2]);
    observation.target = example::accelerator_ref(participants[(i + 1) % 2]);
    observation.region = sol::coherence::MemoryRegionId{"region.pingpong"};
    observation.region_generation = sol::coherence::MemoryRegionGeneration{1};
    observation.state_before = sol::coherence::CoherenceState::Modified;
    observation.state_after = sol::coherence::CoherenceState::Invalid;
    observation.precision = sol::coherence::Precision::ExactEvent;
    observation.granularity = sol::coherence::EvidenceGranularity::Region;
    observation.provenance = sol::coherence::Provenance::SyntheticBackend;
    host->publish(std::move(observation));
  }

  const sol::coherence::Result<std::vector<sol::coherence::Finding>> findings =
      observatory.ping_pong();
  if (!findings.ok()) {
    std::cerr << "error: " << findings.describe() << "\n";
    return 1;
  }
  if (findings.value().empty()) {
    std::cout << "no ping-pong finding\n";
    return 1;
  }
  for (const sol::coherence::Finding& finding : findings.value()) {
    std::cout << sol::coherence::render_text(sol::coherence::explain(finding));
  }
  return 0;
}