// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Benchmarks over completed operations.  Every figure printed here is measured
// on this machine in this run; nothing is estimated.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "coherence/backends/synthetic.hpp"
#include "coherence/backend.hpp"
#include "coherence/explanation.hpp"
#include "coherence/observatory.hpp"
#include "coherence/version.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Measurement {
  std::string name;
  double nanoseconds_per_operation = 0.0;
  std::uint64_t operations = 0;
};

std::vector<Measurement> g_measurements;

void record(const std::string& name, double total_ns, std::uint64_t operations) {
  Measurement measurement;
  measurement.name = name;
  measurement.operations = operations;
  measurement.nanoseconds_per_operation =
      operations == 0 ? 0.0 : total_ns / static_cast<double>(operations);
  g_measurements.push_back(measurement);
  std::printf("%-46s %12llu ops %14.1f ns/op\n", name.c_str(),
              static_cast<unsigned long long>(operations),
              measurement.nanoseconds_per_operation);
}

template <class Fn>
double measure(Fn&& body) {
  const auto begin = Clock::now();
  body();
  const auto end = Clock::now();
  return static_cast<double>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
}

sol::coherence::ObservatoryOptions bench_options() {
  sol::coherence::ObservatoryOptions options;
  options.observer_id = sol::coherence::ObserverId{"observer.bench"};
  options.node_id = sol::coherence::NodeId{"node.bench.0"};
  return options;
}

void register_topology(sol::coherence::Observatory& observatory, std::size_t region_count) {
  sol::coherence::CoherenceDomainRecord coherence_domain;
  coherence_domain.id = sol::coherence::CoherenceDomainId{"cd.bench"};
  coherence_domain.generation = sol::coherence::CoherenceDomainGeneration{1};
  coherence_domain.protocol_family = "synthetic-exact";
  observatory.register_coherence_domain(coherence_domain);

  sol::coherence::MemoryDomainRecord domain;
  domain.id = sol::coherence::MemoryDomainId{"md.bench"};
  domain.generation = sol::coherence::MemoryDomainGeneration{1};
  domain.node = sol::coherence::NodeId{"node.bench.0"};
  domain.kind = sol::coherence::MemoryDomainKind::HostDram;
  domain.coherence_domain = sol::coherence::CoherenceDomainId{"cd.bench"};
  domain.coherent_with_host = true;
  domain.coherent_with_host_known = true;
  observatory.register_memory_domain(domain);

  sol::coherence::ProcessorRecord processor;
  processor.id = sol::coherence::ProcessorId{"cpu.bench"};
  processor.node = sol::coherence::NodeId{"node.bench.0"};
  processor.generation = sol::coherence::DeviceGeneration{1};
  processor.local_memory_domain = sol::coherence::MemoryDomainId{"md.bench"};
  observatory.register_processor(processor);

  sol::coherence::AcceleratorRecord accelerator;
  accelerator.id = sol::coherence::AcceleratorId{"acc.bench"};
  accelerator.node = sol::coherence::NodeId{"node.bench.0"};
  accelerator.generation = sol::coherence::DeviceGeneration{1};
  accelerator.vendor = "synthetic";
  accelerator.local_memory_domain = sol::coherence::MemoryDomainId{"md.bench"};
  observatory.register_accelerator(accelerator);

  for (std::size_t i = 0; i < region_count; ++i) {
    sol::coherence::RegionRecord region;
    region.id = sol::coherence::MemoryRegionId{"region.bench." + std::to_string(i)};
    region.generation = sol::coherence::MemoryRegionGeneration{1};
    region.owner = "benchmark";
    region.memory_domain = sol::coherence::MemoryDomainId{"md.bench"};
    region.memory_domain_generation = sol::coherence::MemoryDomainGeneration{1};
    region.coherence_domain = sol::coherence::CoherenceDomainId{"cd.bench"};
    region.sharing_scope = sol::coherence::SharingScope::DeviceShared;
    region.size_bytes = 4096;
    region.size_known = true;
    region.page_size_bytes = 4096;
    region.page_size_known = true;
    region.allocation_generation = 1;
    region.mapping_generation = 1;
    observatory.register_region(region);
  }

  sol::coherence::TopologyLink link;
  link.from.kind = sol::coherence::ResourceKind::Accelerator;
  link.from.id = sol::coherence::ResourceId{"acc.bench"};
  link.from.generation = 1;
  link.to.kind = sol::coherence::ResourceKind::MemoryDomain;
  link.to.id = sol::coherence::ResourceId{"md.bench"};
  link.to.generation = 1;
  link.locality = sol::coherence::Locality::LocalAccelerator;
  link.provenance = sol::coherence::Provenance::SyntheticBackend;
  observatory.set_topology_link(link);
}

sol::coherence::Observation make_observation(std::size_t region_index, sol::coherence::Nanos timestamp) {
  sol::coherence::Observation observation;
  observation.type = sol::coherence::EventType::RemoteRead;
  observation.timestamp_ns = timestamp;
  observation.direction = sol::coherence::AccessDirection::Read;
  observation.bytes = 64;
  observation.lines = 1;
  sol::coherence::ResourceRef source;
  source.kind = sol::coherence::ResourceKind::Accelerator;
  source.id = sol::coherence::ResourceId{"acc.bench"};
  source.generation = 1;
  observation.source = source;
  sol::coherence::ResourceRef target;
  target.kind = sol::coherence::ResourceKind::MemoryDomain;
  target.id = sol::coherence::ResourceId{"md.bench"};
  target.generation = 1;
  observation.target = target;
  observation.region = sol::coherence::MemoryRegionId{"region.bench." + std::to_string(region_index)};
  observation.region_generation = sol::coherence::MemoryRegionGeneration{1};
  observation.precision = sol::coherence::Precision::ExactEvent;
  observation.granularity = sol::coherence::EvidenceGranularity::Region;
  observation.provenance = sol::coherence::Provenance::SyntheticBackend;
  return observation;
}

void run_scale(std::size_t region_count) {
  std::printf("\n--- regions: %llu ---\n", static_cast<unsigned long long>(region_count));
  sol::coherence::Observatory observatory(bench_options());
  register_topology(observatory, region_count);

  sol::coherence::PublisherRegistration registration;
  registration.id = sol::coherence::PublisherId{"pub.bench"};
  registration.boot = sol::coherence::make_publisher_boot_id();
  registration.provenance = sol::coherence::Provenance::SyntheticBackend;
  sol::coherence::LocalPublisherHost host(observatory, registration);
  if (!host.start().ok()) {
    std::printf("host failed to start\n");
    return;
  }

  const std::uint64_t observations = 20000;
  sol::coherence::Nanos clock = sol::coherence::monotonic_now_ns();
  const double ingest_ns = measure([&]() {
    for (std::uint64_t i = 0; i < observations; ++i) {
      clock += 100;
      host.publish(make_observation(static_cast<std::size_t>(i % region_count), clock));
    }
  });
  record("ingest(observation)", ingest_ns, observations);

  const std::uint64_t batch_count = 40;
  const std::size_t batch_size = 256;
  const double batch_ns = measure([&]() {
    for (std::uint64_t b = 0; b < batch_count; ++b) {
      std::vector<sol::coherence::Observation> batch;
      batch.reserve(batch_size);
      for (std::size_t i = 0; i < batch_size; ++i) {
        clock += 100;
        batch.push_back(make_observation((b * batch_size + i) % region_count, clock));
      }
      host.publish_batch(std::move(batch));
    }
  });
  record("ingest_batch(batch of 256)", batch_ns, batch_count * batch_size);

  const std::uint64_t attribution_count = 2000;
  const double attribution_ns = measure([&]() {
    for (std::uint64_t i = 0; i < attribution_count; ++i) {
      const sol::coherence::Observation observation =
          make_observation(static_cast<std::size_t>(i % region_count), clock);
      observatory.attribute(observation);
    }
  });
  record("attribution(observation)", attribution_ns, attribution_count);

  const std::uint64_t region_lookups = 20000;
  const double lookup_ns = measure([&]() {
    for (std::uint64_t i = 0; i < region_lookups; ++i) {
      observatory.attribute_region(
          sol::coherence::MemoryRegionId{"region.bench." + std::to_string(i % region_count)});
    }
  });
  record("attribute_region(region lookup)", lookup_ns, region_lookups);

  const std::uint64_t snapshots = 20;
  const double snapshot_ns = measure([&]() {
    for (std::uint64_t i = 0; i < snapshots; ++i) {
      sol::coherence::FindingsOptions options;
      options.compute_hotspots = false;
      options.compute_ping_pong = false;
      options.compute_false_sharing = false;
      options.compute_invalidation = false;
      options.compute_remote_access = false;
      options.include_operational = true;
      observatory.snapshot(options);
    }
  });
  record("snapshot(no analyzers)", snapshot_ns, snapshots);

  const double full_snapshot_ns = measure([&]() {
    for (std::uint64_t i = 0; i < 5; ++i) {
      observatory.snapshot();
    }
  });
  record("snapshot(all analyzers)", full_snapshot_ns, 5);

  const double hotspot_ns = measure([&]() {
    for (std::uint64_t i = 0; i < 5; ++i) {
      observatory.hotspots();
    }
  });
  record("hotspots()", hotspot_ns, 5);

  const double pingpong_ns = measure([&]() {
    for (std::uint64_t i = 0; i < 5; ++i) {
      observatory.ping_pong();
    }
  });
  record("ping_pong()", pingpong_ns, 5);

  const double findings_ns = measure([&]() {
    for (std::uint64_t i = 0; i < 5; ++i) {
      observatory.findings();
    }
  });
  record("findings(all analyzers)", findings_ns, 5);

  const sol::coherence::SnapshotPtr snapshot = observatory.snapshot();
  const double render_ns = measure([&]() {
    for (std::uint64_t i = 0; i < 20; ++i) {
      for (const sol::coherence::Finding& finding : snapshot->findings()) {
        sol::coherence::render_text(sol::coherence::explain(finding));
      }
    }
  });
  record("render_explanation(finding)", render_ns,
         static_cast<std::uint64_t>(snapshot->findings().size()) * 20 + 1);

  const std::filesystem::path state_path =
      std::filesystem::temp_directory_path() / "cohobs-bench-state.bin";
  const double save_ns = measure([&]() {
    for (std::uint64_t i = 0; i < 5; ++i) {
      observatory.save_state(state_path);
    }
  });
  record("save_state()", save_ns, 5);

  const double load_ns = measure([&]() {
    for (std::uint64_t i = 0; i < 5; ++i) {
      sol::coherence::Observatory target(bench_options());
      target.load_state(state_path);
    }
  });
  record("load_state()", load_ns, 5);
  std::error_code ignored;
  std::filesystem::remove(state_path, ignored);
}

}  // namespace

int main() {
  std::printf("Coherence Observatory %s benchmarks (build %s)\n",
              std::string(sol::coherence::library_version_string()).c_str(),
              std::string(sol::coherence::build_identity()).c_str());
  for (std::size_t regions : {std::size_t{10}, std::size_t{100}, std::size_t{1000},
                              std::size_t{10000}}) {
    run_scale(regions);
  }
  std::printf("\nAll figures are measured on this host in this run.\n");
  return 0;
}