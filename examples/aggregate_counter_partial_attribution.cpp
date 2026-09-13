// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// A device-scoped counter is exact about the device and cannot resolve a
// region.  The runtime reports an aggregate-only attribution instead of
// manufacturing per-region precision.

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

  sol::coherence::CounterPublication publication;
  publication.counter = sol::coherence::CounterId{"ctr.device.remote_reads"};
  publication.mapped_type = sol::coherence::EventType::RemoteRead;
  publication.kind = sol::coherence::CounterKind::Absolute;
  publication.scope = sol::coherence::CounterScope::PerDevice;
  publication.width_bits = 64;
  publication.generation = sol::coherence::CounterGeneration{1};
  publication.sampling_epoch = sol::coherence::SamplingEpoch{1};
  publication.bytes_per_unit = 64;
  publication.source = example::accelerator_ref("acc.a");
  publication.provenance = sol::coherence::Provenance::HardwarePerformanceCounter;
  publication.granularity = sol::coherence::EvidenceGranularity::Device;

  publication.raw_value = 100000;
  publication.timestamp_ns = sol::coherence::monotonic_now_ns();
  host->publish_counter(publication);
  publication.raw_value = 100512;
  publication.timestamp_ns += 1000000;
  const sol::coherence::Result<sol::coherence::CounterOutcome> outcome =
      host->publish_counter(publication);
  if (!outcome.ok()) {
    std::cerr << "error: " << outcome.describe() << "\n";
    return 1;
  }
  std::cout << "counter delta " << outcome.value().delta << " discontinuity "
            << example::bool_text(outcome.value().discontinuity) << "\n";

  const sol::coherence::SnapshotPtr snapshot = observatory.snapshot();
  for (const auto& entry : snapshot->aggregates().buckets()) {
    if (entry.first.dimension != sol::coherence::AggregateDimension::Device) {
      continue;
    }
    std::cout << "device bucket " << entry.first.value << " observations "
              << entry.second.observations << " bytes " << entry.second.bytes
              << " weakest_precision "
              << sol::coherence::to_string(entry.second.weakest_precision) << "\n";
  }

  const sol::coherence::Result<std::vector<sol::coherence::RemoteAccessRow>> rows =
      observatory.remote_access();
  if (!rows.ok()) {
    std::cerr << "error: " << rows.describe() << "\n";
    return 1;
  }
  for (const sol::coherence::RemoteAccessRow& row : rows.value()) {
    std::cout << "row region " << (row.region.empty() ? "unresolved" : row.region.str())
              << " aggregate_only " << example::bool_text(row.aggregate_only) << " precision "
              << sol::coherence::to_string(row.precision) << "\n";
  }
  return 0;
}