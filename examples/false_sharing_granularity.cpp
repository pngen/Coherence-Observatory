// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// False-sharing-like analysis respects evidence granularity: line-level
// evidence with explicit line identity supports a cache-line claim, page-level
// evidence does not, and missing line identity is reported as insufficient
// granularity rather than guessed.

#include <iostream>

#include "example_support.hpp"

namespace {

sol::coherence::Observation make_write(const char* region, sol::coherence::EvidenceGranularity granularity,
                                  const char* line_index, sol::coherence::Nanos timestamp) {
  sol::coherence::Observation observation;
  observation.type = sol::coherence::EventType::WriteExclusiveTransition;
  observation.timestamp_ns = timestamp;
  observation.direction = sol::coherence::AccessDirection::Write;
  observation.source = example::accelerator_ref("acc.a");
  observation.target = example::domain_ref("md.host");
  observation.region = sol::coherence::MemoryRegionId{region};
  observation.region_generation = sol::coherence::MemoryRegionGeneration{1};
  observation.state_before = sol::coherence::CoherenceState::Shared;
  observation.state_after = sol::coherence::CoherenceState::Exclusive;
  observation.precision = sol::coherence::Precision::ExactEvent;
  observation.granularity = granularity;
  observation.provenance = sol::coherence::Provenance::SyntheticBackend;
  if (line_index != nullptr) {
    observation.metadata.add("line.index", line_index);
  }
  return observation;
}

}  // namespace

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
  for (int i = 0; i < 6; ++i) {
    host->publish(make_write("region.line", sol::coherence::EvidenceGranularity::CacheLine, "0",
                             clock));
    clock += 1000;

    sol::coherence::Observation invalidation =
        make_write("region.line", sol::coherence::EvidenceGranularity::CacheLine, "0", clock);
    invalidation.type = sol::coherence::EventType::Invalidation;
    invalidation.source = example::accelerator_ref("acc.b");
    invalidation.target = example::accelerator_ref("acc.a");
    invalidation.state_before = sol::coherence::CoherenceState::Shared;
    invalidation.state_after = sol::coherence::CoherenceState::Invalid;
    host->publish(std::move(invalidation));
    clock += 1000;

    host->publish(make_write("region.shared", sol::coherence::EvidenceGranularity::Page, nullptr,
                             clock));
    clock += 1000;
  }

  const sol::coherence::Result<std::vector<sol::coherence::Finding>> findings =
      observatory.false_sharing();
  if (!findings.ok()) {
    std::cerr << "error: " << findings.describe() << "\n";
    return 1;
  }
  for (const sol::coherence::Finding& finding : findings.value()) {
    std::cout << finding.subject << " granularity "
              << sol::coherence::to_string(finding.granularity) << " classification "
              << sol::coherence::to_string(finding.contention) << "\n";
    for (const std::string& missing : finding.missing_evidence) {
      std::cout << "  missing: " << missing << "\n";
    }
  }
  return findings.value().empty() ? 1 : 0;
}