// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// A fenced publisher boot loses authority permanently: its evidence stops
// being current, stale replay is rejected, and a replacement must register
// under a fresh boot identity.

#include <iostream>

#include "example_support.hpp"
#include "coherence/client.hpp"

int main() {
  sol::coherence::Observatory observatory(example::default_options());
  example::register_topology(observatory);

  std::unique_ptr<sol::coherence::LocalPublisherHost> storage;
  sol::coherence::LocalPublisherHost* host = nullptr;
  if (!example::start_host(observatory, &host, &storage).ok()) {
    std::cerr << "error: could not start publisher host\n";
    return 1;
  }

  sol::coherence::Observation observation;
  observation.type = sol::coherence::EventType::SharedRead;
  observation.timestamp_ns = sol::coherence::monotonic_now_ns();
  observation.direction = sol::coherence::AccessDirection::Read;
  observation.bytes = 64;
  observation.source = example::accelerator_ref("acc.a");
  observation.target = example::domain_ref("md.host");
  observation.region = sol::coherence::MemoryRegionId{"region.shared"};
  observation.region_generation = sol::coherence::MemoryRegionGeneration{1};
  observation.precision = sol::coherence::Precision::ExactEvent;
  observation.granularity = sol::coherence::EvidenceGranularity::Region;
  observation.provenance = sol::coherence::Provenance::SyntheticBackend;
  const sol::coherence::Result<sol::coherence::IngestionOutcome> first = host->publish(observation);
  std::cout << "before fence: " << sol::coherence::to_string(first.value().disposition) << "\n";

  const sol::coherence::Result<sol::coherence::PublisherView> fenced =
      observatory.fence_publisher(sol::coherence::PublisherId{"pub.example.1"},
                                  sol::coherence::FenceReason::ConnectionClosed,
                                  "example: publisher process ended");
  if (!fenced.ok()) {
    std::cerr << "error: " << fenced.describe() << "\n";
    return 1;
  }
  std::cout << "fenced current=" << example::bool_text(fenced.value().current)
            << " reason=" << sol::coherence::to_string(fenced.value().fence_reason) << "\n";

  // The stale boot can never publish again.
  observation.timestamp_ns += 1000;
  const sol::coherence::Result<sol::coherence::IngestionOutcome> replay = host->publish(observation);
  std::cout << "replay after fence: "
            << (replay.value().disposition == sol::coherence::IngestionDisposition::Rejected
                    ? "REJECTED"
                    : "ACCEPTED")
            << " code " << sol::coherence::to_string(replay.value().code) << "\n";

  // A replacement publisher must present a fresh boot identity.
  sol::coherence::PublisherRegistration replacement;
  replacement.id = sol::coherence::PublisherId{"pub.example.1"};
  replacement.boot = sol::coherence::PublisherBootId{0};
  replacement.provenance = sol::coherence::Provenance::SyntheticBackend;
  const sol::coherence::Result<sol::coherence::PublisherView> refused =
      observatory.register_publisher(replacement);
  std::cout << "replacement without a boot identity: "
            << (refused.ok() ? "accepted" : "rejected") << "\n";

  replacement.boot = sol::coherence::make_publisher_boot_id();
  const sol::coherence::Result<sol::coherence::PublisherView> accepted =
      observatory.register_publisher(replacement);
  std::cout << "replacement with a fresh boot identity: "
            << (accepted.ok() ? "accepted" : "rejected") << "\n";
  return accepted.ok() ? 0 : 1;
}