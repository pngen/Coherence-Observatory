// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Real CPU / operating-system backend.
//
// This backend reports ONLY what the operating system genuinely exposes:
// processor topology, NUMA nodes, cache hierarchy geometry, processor groups,
// installed memory and real measured access latency.  On this host there is no
// OS interface that reports cache-line ownership or invalidation counts; those
// capabilities are classified UNSUPPORTED rather than synthesised here.

#ifndef COHERENCE_BACKENDS_CPU_OS_HPP
#define COHERENCE_BACKENDS_CPU_OS_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "coherence/backend.hpp"

namespace sol::coherence {

/// One NUMA node discovered from the operating system.
struct COHERENCE_API HostNumaNode {
  std::uint32_t index = 0;
  std::uint32_t processor_count = 0;
  std::uint64_t memory_bytes = 0;
  bool memory_known = false;
  std::vector<std::uint32_t> logical_processors;
};

/// One cache level discovered from the operating system.
struct COHERENCE_API HostCache {
  std::uint8_t level = 0;
  std::uint32_t line_size_bytes = 0;
  std::uint64_t size_bytes = 0;
  std::uint32_t logical_processor_count = 0;
  /// True when the OS reports this cache as shared between cores.
  bool shared = false;
};

/// Real host topology as reported by the OS.
struct COHERENCE_API HostTopology {
  std::string processor_name;
  std::string architecture;
  std::uint32_t physical_core_count = 0;
  std::uint32_t logical_processor_count = 0;
  std::uint32_t processor_group_count = 0;
  std::uint64_t total_physical_memory_bytes = 0;
  std::vector<HostNumaNode> numa_nodes;
  std::vector<HostCache> caches;
  /// Mechanisms actually used, for capability reporting.
  std::vector<std::string> mechanisms;
};

/// Discovers real host topology.  Returns UnsupportedCapability when no
/// discovery mechanism is available on this platform.
COHERENCE_API Result<HostTopology> discover_host_topology();

/// CPU/OS backend configuration.
struct COHERENCE_API CpuOsConfig {
  /// Register discovered processors, memory domains and topology links.
  bool register_topology = true;
  /// Register a memory region per NUMA node describing its local memory.
  bool register_memory_regions = true;
  /// Run a real placement-controlled memory workload and publish the measured
  /// results.  The published evidence is REAL but derived from OS placement
  /// and thread affinity, never from a coherence counter.
  bool run_workload = false;
  std::uint64_t workload_bytes = 32ull << 20;
  std::uint32_t workload_passes = 4;
  std::uint32_t locality_probe_samples = 64;
  std::size_t stride_bytes = 64;
  std::string publisher_id = "pub.cpu_os.local";
  std::string publisher_name = "cpu-os-collector";
};

/// Real CPU/OS collector.
class COHERENCE_API CpuOsBackend : public Collector {
 public:
  explicit CpuOsBackend(CpuOsConfig config = {});
  ~CpuOsBackend() override;

  CpuOsBackend(const CpuOsBackend&) = delete;
  CpuOsBackend& operator=(const CpuOsBackend&) = delete;

  std::string_view name() const noexcept override;
  std::vector<Capability> capabilities() const override;
  Status start(const CollectorContext& context) override;
  Status poll(const CollectorContext& context) override;
  Status stop() override;

  /// Topology discovered at start(); empty before start.
  const HostTopology& topology() const noexcept;

  /// Measured local-memory latency in nanoseconds, or negative when unmeasured.
  double measured_local_latency_ns() const noexcept;
  /// Measured cross-node latency in nanoseconds, or negative when the host has
  /// a single NUMA node or the probe could not run.
  double measured_remote_latency_ns() const noexcept;
  /// Observations published.
  std::uint64_t emissions() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sol::coherence

#endif  // COHERENCE_BACKENDS_CPU_OS_HPP
