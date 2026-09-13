// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Publishes one exact synthetic observation and shows the attribution that
// results.  Expected outcome: ATTRIBUTED_EXACT bound to the region generation.

#include <iostream>

#include "example_support.hpp"

int main() {
  sol::coherence::Observatory observatory(example::default_options());
  example::register_topology(observatory);

  std::unique_ptr<sol::coherence::LocalPublisherHost> storage;
  sol::coherence::LocalPublisherHost* host = nullptr;
  const sol::coherence::Status started = example::start_host(observatory, &host, &storage);
  if (!started.ok()) {
    std::cerr << "error: " << started.describe() << "\n";
    return 1;
  }

  sol::coherence::Observation observation;
  observation.type = sol::coherence::EventType::RemoteRead;
  observation.timestamp_ns = sol::coherence::monotonic_now_ns();
  observation.direction = sol::coherence::AccessDirection::Read;
  observation.bytes = 4096;
  observation.pages = 1;
  observation.source = example::accelerator_ref("acc.a");
  observation.target = example::domain_ref("md.host");
  observation.region = sol::coherence::MemoryRegionId{"region.shared"};
  observation.region_generation = sol::coherence::MemoryRegionGeneration{1};
  observation.precision = sol::coherence::Precision::ExactEvent;
  observation.granularity = sol::coherence::EvidenceGranularity::Region;
  observation.provenance = sol::coherence::Provenance::SyntheticBackend;

  const sol::coherence::Result<sol::coherence::IngestionOutcome> outcome = host->publish(observation);
  if (!outcome.ok()) {
    std::cerr << "error: " << outcome.describe() << "\n";
    return 1;
  }
  std::cout << "disposition " << sol::coherence::to_string(outcome.value().disposition) << "\n";

  const sol::coherence::Result<sol::coherence::AttributionResult> attribution =
      observatory.attribute_region(sol::coherence::MemoryRegionId{"region.shared"});
  if (!attribution.ok()) {
    std::cerr << "error: " << attribution.describe() << "\n";
    return 1;
  }
  std::cout << sol::coherence::render_text(sol::coherence::explain(attribution.value()));
  return attribution.value().outcome == sol::coherence::AttributionOutcome::AttributedExact ? 0 : 1;
}