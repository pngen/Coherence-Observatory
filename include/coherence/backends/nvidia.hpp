// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// NVIDIA device backend.
//
// Only genuinely observable facts are reported: device identity, UUID, memory
// capacity, driver version, compute capability, peer-to-peer capability as
// reported by the vendor API, and the measured cost of real CUDA memory
// operations the backend itself performs.
//
// Executing a CUDA transfer does NOT prove anything about coherence, and this
// backend never claims otherwise.  Cache-line ownership, invalidation counts
// and GPU peer-coherence semantics are classified UNSUPPORTED unless a vendor
// API actually exposes them.

#ifndef COHERENCE_BACKENDS_NVIDIA_HPP
#define COHERENCE_BACKENDS_NVIDIA_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "coherence/backend.hpp"

namespace sol::coherence {

/// One discovered NVIDIA device.
struct COHERENCE_API NvidiaDevice {
  std::uint32_t index = 0;
  std::string name;
  std::string uuid;
  std::string driver_version;
  std::uint64_t memory_total_bytes = 0;
  std::uint64_t memory_free_bytes = 0;
  std::int32_t compute_capability_major = 0;
  std::int32_t compute_capability_minor = 0;
  std::string pci_bus_id;
  /// Peer-to-peer capability as reported by the vendor API, when known.
  std::vector<std::string> p2p_capable_peers;
  bool p2p_capability_known = false;
};

/// Discovery result plus the mechanisms that produced it.
struct COHERENCE_API NvidiaDiscovery {
  bool nvml_available = false;
  bool cuda_driver_available = false;
  std::string nvml_version;
  std::string cuda_driver_version;
  std::vector<NvidiaDevice> devices;
  std::vector<std::string> mechanisms;
  std::string unavailable_reason;
};

/// Discovers NVIDIA devices through the vendor APIs, loading them dynamically.
/// Never fails hard: an unavailable API is reported in the result.
COHERENCE_API NvidiaDiscovery discover_nvidia();

/// Result of a real CUDA memory workload.
struct COHERENCE_API CudaWorkloadResult {
  bool executed = false;
  bool kernel_executed = false;
  bool parity_verified = false;
  std::string failure_reason;
  std::uint64_t bytes_transferred = 0;
  std::uint64_t kernel_launches = 0;
  double host_to_device_ns = 0.0;
  double device_to_host_ns = 0.0;
  double kernel_ns = 0.0;
  double achieved_host_to_device_bytes_per_second = 0.0;
  double achieved_device_to_host_bytes_per_second = 0.0;
};

/// NVIDIA backend configuration.
struct COHERENCE_API NvidiaConfig {
  bool run_workload = false;
  std::uint64_t workload_bytes = 64ull << 20;
  std::uint32_t device_index = 0;
  std::string publisher_id = "pub.nvidia.local";
  std::string publisher_name = "nvidia-collector";
};

/// NVIDIA collector.  Publishes REAL device discovery and REAL measured
/// transfer costs, and explicitly publishes the absence of coherence
/// telemetry as an UNSUPPORTED capability.
class COHERENCE_API NvidiaBackend : public Collector {
 public:
  explicit NvidiaBackend(NvidiaConfig config = {});
  ~NvidiaBackend() override;

  NvidiaBackend(const NvidiaBackend&) = delete;
  NvidiaBackend& operator=(const NvidiaBackend&) = delete;

  std::string_view name() const noexcept override;
  std::vector<Capability> capabilities() const override;
  Status start(const CollectorContext& context) override;
  Status poll(const CollectorContext& context) override;
  Status stop() override;

  const NvidiaDiscovery& discovery() const noexcept;
  const CudaWorkloadResult& workload() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sol::coherence

#endif  // COHERENCE_BACKENDS_NVIDIA_HPP
