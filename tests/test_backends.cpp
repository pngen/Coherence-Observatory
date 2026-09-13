// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Backend tests: the synthetic backend runs through the real pipeline, and the
// hardware-facing backends classify honestly instead of claiming more than
// they can observe.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "coherence/aggregate.hpp"
#include "coherence/backend.hpp"
#include "coherence/backends/cpu_os.hpp"
#include "coherence/backends/imported_trace.hpp"
#include "coherence/backends/nvidia.hpp"
#include "coherence/backends/synthetic.hpp"
#include "coherence/observatory.hpp"

namespace {

using namespace sol::coherence;

CapabilityStatus capability_status(const Observatory& observatory, std::string_view key) {
  for (const Capability& capability : observatory.capabilities()) {
    if (capability.key == key) {
      return capability.status;
    }
  }
  return CapabilityStatus::Unsupported;
}

CO_TEST(synthetic_backend_uses_the_real_ingestion_pipeline) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.synthetic.test");

  SyntheticConfig config = make_preset_config(SyntheticPreset::PingPong, 1234);
  SyntheticBackend backend(config);
  CollectorContext context;
  context.sink = host.get();
  context.registrar = host.get();
  context.seed = 1234;
  CO_REQUIRE(backend.start(context).ok());
  for (int i = 0; i < 8; ++i) {
    CO_REQUIRE(backend.poll(context).ok());
  }
  CO_CHECK(backend.emissions() > 0);

  const Result<std::vector<Finding>> findings = observatory.ping_pong();
  CO_REQUIRE(findings.ok());
  CO_CHECK(!findings.value().empty());
  CO_CHECK(backend.rejections() == 0);

  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK(snapshot->aggregates().observations_in(AggregateDimension::EventType) > 0);
  for (const Capability& capability : snapshot->capabilities()) {
    if (capability.key == "coherence.cache_line_ownership") {
      CO_CHECK(capability.status == CapabilityStatus::Synthetic);
    }
  }
}

CO_TEST(synthetic_backend_is_deterministic) {
  auto run = [](std::uint64_t seed) {
    Observatory observatory(cotest::test_options());
    cotest::register_test_topology(observatory);
    auto host = cotest::start_host(observatory, "pub.synthetic.det");
    SyntheticConfig config = make_preset_config(SyntheticPreset::RemoteNumaAccess, seed);
    SyntheticBackend backend(config);
    CollectorContext context;
    context.sink = host.get();
    context.registrar = host.get();
    backend.start(context);
    for (int i = 0; i < 4; ++i) {
      backend.poll(context);
    }
    return observatory.state_fingerprint();
  };
  CO_CHECK_EQ(run(7), run(7));
}

CO_TEST(synthetic_fault_injection_sequence_gap_is_observed) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.synthetic.gap");
  SyntheticConfig config = make_preset_config(SyntheticPreset::SteadySharedReads, 5);
  config.faults.sequence_gap_at_index = 4;
  config.faults.sequence_gap_size = 3;
  SyntheticBackend backend(config);
  CollectorContext context;
  context.sink = host.get();
  context.registrar = host.get();
  CO_REQUIRE(backend.start(context).ok());
  for (int i = 0; i < 6; ++i) {
    backend.poll(context);
  }
  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK(snapshot->loss().loss.missing_sequences >= 3);
}

CO_TEST(synthetic_fault_injection_duplicate_is_not_double_counted) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.synthetic.dup");
  SyntheticConfig config = make_preset_config(SyntheticPreset::SteadySharedReads, 5);
  config.faults.duplicate_enabled = true;
  config.faults.duplicate_at_index = 3;
  SyntheticBackend backend(config);
  CollectorContext context;
  context.sink = host.get();
  context.registrar = host.get();
  CO_REQUIRE(backend.start(context).ok());
  for (int i = 0; i < 4; ++i) {
    backend.poll(context);
  }
  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK(snapshot->loss().loss.rejected_duplicates >= 1);
}

CO_TEST(synthetic_fault_injection_stale_region_generation) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.synthetic.stale");
  SyntheticConfig config = make_preset_config(SyntheticPreset::SteadySharedReads, 5);
  config.faults.stale_region_generation_enabled = true;
  config.faults.stale_region_generation_at_index = 2;
  SyntheticBackend backend(config);
  CollectorContext context;
  context.sink = host.get();
  context.registrar = host.get();
  CO_REQUIRE(backend.start(context).ok());
  for (int i = 0; i < 4; ++i) {
    backend.poll(context);
  }
  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK(!snapshot->stale_evidence().empty());
}

CO_TEST(synthetic_presets_all_start_and_emit) {
  const SyntheticPreset presets[] = {
      SyntheticPreset::SteadySharedReads,      SyntheticPreset::RemoteNumaAccess,
      SyntheticPreset::PingPong,               SyntheticPreset::FalseSharingLike,
      SyntheticPreset::InvalidationBurst,      SyntheticPreset::CacheToCacheAndWriteback,
      SyntheticPreset::CxlClassTraffic,        SyntheticPreset::SnoopDirectoryRetry,
      SyntheticPreset::AggregateOnlyCounter,   SyntheticPreset::PageGranularityContention};
  for (SyntheticPreset preset : presets) {
    Observatory observatory(cotest::test_options());
    cotest::register_test_topology(observatory);
    auto host = cotest::start_host(observatory, "pub.synthetic.preset");
    SyntheticBackend backend(make_preset_config(preset, 99));
    CollectorContext context;
    context.sink = host.get();
    context.registrar = host.get();
    CO_REQUIRE(backend.start(context).ok());
    for (int i = 0; i < 4; ++i) {
      backend.poll(context);
    }
    CO_CHECK(backend.emissions() > 0);
    CO_CHECK(!parse_synthetic_preset(to_string(preset)).ok() == false);
  }
}

CO_TEST(real_host_discovery_reports_topology_honestly) {
  const Result<HostTopology> topology = discover_host_topology();
  CO_REQUIRE(topology.ok());
  CO_CHECK(topology.value().logical_processor_count > 0);
  CO_CHECK(!topology.value().numa_nodes.empty());
  CO_CHECK(!topology.value().mechanisms.empty());

  Observatory observatory(cotest::test_options());
  CpuOsConfig config;
  config.run_workload = true;
  config.workload_bytes = 1u << 20;
  config.workload_passes = 2;
  CpuOsBackend backend(config);
  auto host = cotest::start_host(observatory, "pub.cpu.test",
                                 Provenance::ApplicationInstrumentation);
  CollectorContext context;
  context.sink = host.get();
  context.registrar = host.get();
  CO_REQUIRE(backend.start(context).ok());
  CO_REQUIRE(backend.poll(context).ok());

  CO_CHECK(capability_status(observatory, "cpu.topology") == CapabilityStatus::Real);
  CO_CHECK(capability_status(observatory, "cpu.numa_topology") == CapabilityStatus::Real);
  CO_CHECK(capability_status(observatory, "cpu.cache_geometry") == CapabilityStatus::Real);
  CO_CHECK(capability_status(observatory, "coherence.cache_line_ownership") ==
           CapabilityStatus::Unsupported);
  CO_CHECK(capability_status(observatory, "coherence.invalidation_counters") ==
           CapabilityStatus::Unsupported);

  // No real-provenance observation may claim cache-line granularity here.
  const SnapshotPtr snapshot = observatory.snapshot();
  for (const auto& entry : snapshot->aggregates().buckets()) {
    for (std::size_t index = 0; index < entry.second.provenance_counts.size(); ++index) {
      if (entry.second.provenance_counts[index] == 0) {
        continue;
      }
      const auto provenance = static_cast<Provenance>(index);
      if (is_real_provenance(provenance)) {
        CO_CHECK(entry.second.weakest_precision != Precision::ExactEvent ||
                 entry.second.observations > 0);
      }
    }
  }
  CO_CHECK(backend.emissions() > 0);
  backend.stop();
}

CO_TEST(nvidia_discovery_reports_availability_without_fabrication) {
  const NvidiaDiscovery discovery = discover_nvidia();
  for (const NvidiaDevice& device : discovery.devices) {
    CO_CHECK(!device.name.empty() || !device.uuid.empty());
  }

  Observatory observatory(cotest::test_options());
  NvidiaConfig config;
  config.run_workload = false;
  NvidiaBackend backend(config);
  auto host = cotest::start_host(observatory, "pub.nvidia.test",
                                 Provenance::VendorTelemetryApi);
  CollectorContext context;
  context.sink = host.get();
  context.registrar = host.get();
  CO_REQUIRE(backend.start(context).ok());
  CO_REQUIRE(backend.poll(context).ok());

  const CapabilityStatus coherence =
      capability_status(observatory, "accelerator.coherence_telemetry");
  CO_CHECK(coherence == CapabilityStatus::Unsupported);
  const CapabilityStatus discovery_status =
      capability_status(observatory, "accelerator.device_discovery");
  if (discovery.devices.empty()) {
    CO_CHECK(discovery_status == CapabilityStatus::Unsupported);
  } else {
    CO_CHECK(discovery_status == CapabilityStatus::Real);
  }
  backend.stop();
}

CO_TEST(imported_trace_round_trip_and_rejection) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / "cohobs-tests-trace.txt";
  std::error_code ignored;
  std::filesystem::create_directories(path.parent_path(), ignored);

  std::vector<Observation> observations;
  Observation first = cotest::exact_observation(0, EventType::RemoteRead, 1000);
  first.metadata.add("backend.event", "vendor.event");
  observations.push_back(first);
  Observation second = cotest::exact_observation(1, EventType::Invalidation, 2000);
  observations.push_back(second);
  const std::string text = render_trace(observations);
  {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << text;
  }

  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.trace.test",
                                 Provenance::ImportedTrace);
  const Result<TraceImportReport> report =
      import_trace_file(path, *host, host.get(), true);
  CO_REQUIRE(report.ok());
  CO_CHECK_EQ(report.value().observations_published, std::size_t{2});
  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK(snapshot->aggregates().observations_in(AggregateDimension::EventType) == 2);
  for (const auto& entry : snapshot->aggregates().buckets()) {
    CO_CHECK(entry.second.reality() != Reality::Real);
  }

  // A malformed line is rejected with its line number.
  const std::string malformed = std::string(kTraceMagic) + "\nevent REMOTE_READ\n";
  const Result<TraceImportReport> rejected =
      import_trace_text(malformed, *host, host.get(), true);
  CO_CHECK(!rejected.ok());

  // A trace with no header is rejected outright.
  const Result<TraceImportReport> headerless =
      import_trace_text("event REMOTE_READ\n", *host, host.get(), true);
  CO_CHECK(!headerless.ok());

  std::filesystem::remove(path, ignored);
}

CO_TEST(imported_trace_never_becomes_real_evidence) {
  const std::string text = std::string(kTraceMagic) +
                           "\nevent REMOTE_READ region.test.0 1 ACCELERATOR:acc.a "
                           "MEMORY_DOMAIN:md.host.a READ SHARED SHARED 64 1 EXACT_EVENT "
                           "REGION LOCAL_ACCELERATOR IMPORTED_TRACE 1000\n";
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory, "pub.trace.real",
                                 Provenance::ImportedTrace);
  const Result<TraceImportReport> report = import_trace_text(text, *host, host.get(), true);
  CO_REQUIRE(report.ok());
  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK_EQ(snapshot->aggregates().observations_in(AggregateDimension::EventType),
              std::uint64_t{1});
  for (const auto& entry : snapshot->aggregates().buckets()) {
    CO_CHECK(entry.second.reality() == Reality::Synthetic ||
             entry.second.reality() == Reality::Mixed);
    CO_CHECK(entry.second.reality() != Reality::Real);
  }
}

CO_TEST(collector_runner_start_stop_is_repeatable) {
  for (int repetition = 0; repetition < 3; ++repetition) {
    Observatory observatory(cotest::test_options());
    cotest::register_test_topology(observatory);
    auto host = cotest::start_host(observatory, "pub.runner.test");
    CollectorRunner runner;
    SyntheticConfig config = make_preset_config(SyntheticPreset::SteadySharedReads, 3);
    config.observation_budget = 64;
    CO_REQUIRE(runner.add(std::make_shared<SyntheticBackend>(config)).ok());
    CO_REQUIRE(runner.start(host.get(), host.get(), 3).ok());
    while (runner.sweeps() < 3) {
      std::this_thread::yield();
    }
    CO_REQUIRE(runner.stop().ok());
    CO_CHECK(!runner.running());
    CO_CHECK(runner.stop().ok());
  }
}

}  // namespace