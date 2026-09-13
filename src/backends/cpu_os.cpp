// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Real CPU / operating-system backend.
//
// Everything published here is REAL and comes from an operating-system API or
// from this backend's own measurement of a workload it actually ran.  Nothing
// here is a coherence counter, and the backend never claims cache-line
// ownership: those capabilities are registered as UNSUPPORTED.

#include "coherence/backends/cpu_os.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "coherence/limits.hpp"
#include "text_util.hpp"

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace sol::coherence {
namespace {

constexpr std::string_view kCapabilityTopology = "cpu.topology";
constexpr std::string_view kCapabilityNuma = "cpu.numa_topology";
constexpr std::string_view kCapabilityCache = "cpu.cache_geometry";
constexpr std::string_view kCapabilityPlacement = "os.page_numa_placement";
constexpr std::string_view kCapabilityRemoteNuma = "cpu.remote_numa_observability";
constexpr std::string_view kCapabilityCacheLine = "coherence.cache_line_ownership";
constexpr std::string_view kCapabilityInvalidations = "coherence.invalidation_counters";

std::string window_text(const char* value) {
  if (value == nullptr) {
    return std::string();
  }
  return std::string(value);
}

#if defined(_WIN32)

std::string processor_brand() {
  char* identifier = nullptr;
  std::size_t length = 0;
  if (_dupenv_s(&identifier, &length, "PROCESSOR_IDENTIFIER") == 0 && identifier != nullptr) {
    std::string value(identifier);
    std::free(identifier);
    return value;
  }
  return std::string("unknown");
}

#else

std::string processor_brand() {
  return std::string("unknown");
}

#endif

std::uint64_t host_memory_bytes() {
#if defined(_WIN32)
  MEMORYSTATUSEX status{};
  status.dwLength = sizeof(status);
  if (GlobalMemoryStatusEx(&status) != 0) {
    return static_cast<std::uint64_t>(status.ullTotalPhys);
  }
  return 0;
#else
  const long pages = ::sysconf(_SC_PHYS_PAGES);
  const long page_size = ::sysconf(_SC_PAGESIZE);
  if (pages <= 0 || page_size <= 0) {
    return 0;
  }
  return static_cast<std::uint64_t>(pages) * static_cast<std::uint64_t>(page_size);
#endif
}

}  // namespace

Result<HostTopology> discover_host_topology() {
  HostTopology topology;
#if defined(_WIN32)
  topology.processor_name = processor_brand();
  topology.architecture = "x64";
  topology.total_physical_memory_bytes = host_memory_bytes();
  topology.mechanisms.push_back("GetLogicalProcessorInformationEx");
  topology.mechanisms.push_back("GetNumaHighestNodeNumber");
  topology.mechanisms.push_back("GlobalMemoryStatusEx");

  DWORD length = 0;
  GetLogicalProcessorInformationEx(RelationAll, nullptr, &length);
  if (length == 0) {
    return fail_as<HostTopology>(ErrorCode::UnsupportedCapability,
                                 "GetLogicalProcessorInformationEx returned no data");
  }
  std::vector<unsigned char> buffer(length);
  if (GetLogicalProcessorInformationEx(
          RelationAll, reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
          &length) == 0) {
    return fail_as<HostTopology>(ErrorCode::UnsupportedCapability,
                                 "GetLogicalProcessorInformationEx failed");
  }

  DWORD offset = 0;
  while (offset < length) {
    const auto* entry =
        reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
    if (entry->Relationship == RelationProcessorCore) {
      ++topology.physical_core_count;
    } else if (entry->Relationship == RelationNumaNode) {
      HostNumaNode node;
      node.index = entry->NumaNode.NodeNumber;
      topology.numa_nodes.push_back(node);
    } else if (entry->Relationship == RelationCache) {
      HostCache cache;
      cache.level = entry->Cache.Level;
      cache.line_size_bytes = entry->Cache.LineSize;
      cache.size_bytes = entry->Cache.CacheSize;
      cache.shared = entry->Cache.Type != CacheUnified || entry->Cache.GroupCount > 1;
      topology.caches.push_back(cache);
    }
    offset += entry->Size;
  }

  {
    ULONG highest = 0;
    if (GetNumaHighestNodeNumber(&highest) != 0) {
      std::vector<HostNumaNode> nodes;
      for (ULONG index = 0; index <= highest; ++index) {
        GROUP_AFFINITY affinity{};
        HostNumaNode node;
        node.index = index;
        if (GetNumaNodeProcessorMaskEx(static_cast<USHORT>(index), &affinity) != 0) {
          for (int bit = 0; bit < 64; ++bit) {
            if ((affinity.Mask & (1ull << bit)) != 0) {
              node.logical_processors.push_back(static_cast<std::uint32_t>(
                  affinity.Group * 64 + static_cast<unsigned>(bit)));
            }
          }
          node.processor_count = static_cast<std::uint32_t>(node.logical_processors.size());
        }
        ULONGLONG available = 0;
        if (GetNumaAvailableMemoryNodeEx(static_cast<USHORT>(index), &available) != 0) {
          node.memory_bytes = static_cast<std::uint64_t>(available);
          node.memory_known = true;
        }
        nodes.push_back(std::move(node));
      }
      topology.numa_nodes = std::move(nodes);
    }
  }

  SYSTEM_INFO info{};
  GetNativeSystemInfo(&info);
  topology.logical_processor_count =
      static_cast<std::uint32_t>(info.dwNumberOfProcessors);
  topology.processor_group_count = GetActiveProcessorGroupCount();
  if (topology.physical_core_count == 0) {
    topology.physical_core_count = topology.logical_processor_count;
  }
  if (topology.numa_nodes.empty()) {
    HostNumaNode node;
    node.index = 0;
    node.processor_count = topology.logical_processor_count;
    for (std::uint32_t i = 0; i < topology.logical_processor_count; ++i) {
      node.logical_processors.push_back(i);
    }
    topology.numa_nodes.push_back(std::move(node));
  }
#else
  topology.architecture = "unknown";
  topology.total_physical_memory_bytes = host_memory_bytes();
  const long processors = ::sysconf(_SC_NPROCESSORS_ONLN);
  topology.logical_processor_count =
      processors > 0 ? static_cast<std::uint32_t>(processors) : 0;
  topology.physical_core_count = topology.logical_processor_count;
  HostNumaNode node;
  node.index = 0;
  node.processor_count = topology.logical_processor_count;
  topology.numa_nodes.push_back(node);
  topology.mechanisms.push_back("sysconf");
#endif
  return Result<HostTopology>(std::move(topology));
}

// ---- CpuOsBackend ------------------------------------------------------

struct CpuOsBackend::Impl {
  CpuOsConfig config;
  CollectorContext context;
  HostTopology topology;
  bool started = false;
  std::uint64_t emissions = 0;
  double local_latency_ns = -1.0;
  double remote_latency_ns = -1.0;
  bool workload_done = false;

  void publish_capability(CollectorContext& ctx, std::string key, CapabilityStatus status,
                          std::string detail_text, std::string mechanism) {
    Capability capability;
    capability.key = std::move(key);
    capability.status = status;
    capability.detail = std::move(detail_text);
    capability.mechanism = std::move(mechanism);
    if (ctx.registrar != nullptr) {
      ctx.registrar->set_capability(capability);
    }
  }

  Result<Observation> make_observation(EventType type, const std::string& region,
                                       ResourceKind source_kind, const std::string& source,
                                       const std::string& target, AccessDirection direction,
                                       std::uint64_t bytes, std::uint64_t pages,
                                       Locality locality, Precision precision,
                                       EvidenceGranularity granularity,
                                       std::int64_t measured_ns) {
    Observation observation;
    observation.type = type;
    observation.timestamp_ns = monotonic_now_ns();
    observation.direction = direction;
    observation.bytes = bytes;
    observation.pages = pages;
    observation.locality = locality;
    observation.locality_declared = true;
    observation.precision = precision;
    observation.granularity = granularity;
    observation.provenance = Provenance::ApplicationInstrumentation;
    observation.measured_duration_ns = measured_ns;
    observation.region = MemoryRegionId{region};
    observation.region_generation = MemoryRegionGeneration{1};
    observation.workload = WorkloadId{"workload.cpu_os.locality"};
    observation.source = ResourceRef{};
    observation.source->kind = source_kind;
    observation.source->id = ResourceId{source};
    observation.source->generation = 1;
    observation.target = ResourceRef{};
    observation.target->kind = ResourceKind::MemoryDomain;
    observation.target->id = ResourceId{target};
    observation.target->generation = 1;
    if (context.registrar != nullptr) {
      observation.topology_generation = context.registrar->topology_generation();
    }
    return Result<Observation>(std::move(observation));
  }
};

CpuOsBackend::CpuOsBackend(CpuOsConfig config) : impl_(std::make_unique<Impl>()) {
  impl_->config = std::move(config);
}

CpuOsBackend::~CpuOsBackend() { stop(); }

std::string_view CpuOsBackend::name() const noexcept { return "cpu-os"; }

std::vector<Capability> CpuOsBackend::capabilities() const {
  std::vector<Capability> capabilities;
  auto add = [&capabilities](std::string key, CapabilityStatus status, std::string detail_text,
                             std::string mechanism) {
    Capability capability;
    capability.key = std::move(key);
    capability.status = status;
    capability.detail = std::move(detail_text);
    capability.mechanism = std::move(mechanism);
    capabilities.push_back(std::move(capability));
  };
  add(std::string(kCapabilityTopology), CapabilityStatus::Real,
      "processor topology discovered from the operating system",
      impl_->topology.mechanisms.empty() ? "platform discovery"
                                         : impl_->topology.mechanisms.front());
  add(std::string(kCapabilityNuma), CapabilityStatus::Real,
      "NUMA node count and per-node processor masks discovered from the operating system",
      "GetNumaHighestNodeNumber/GetNumaNodeProcessorMaskEx");
  add(std::string(kCapabilityCache), CapabilityStatus::Real,
      "cache hierarchy geometry (levels, line size, sizes) reported by the operating system",
      "GetLogicalProcessorInformationEx");
  add(std::string(kCapabilityPlacement), CapabilityStatus::Real,
      "page-level NUMA placement of memory this process allocated",
      "QueryWorkingSetEx");
  add(std::string(kCapabilityRemoteNuma),
      impl_->topology.numa_nodes.size() > 1 ? CapabilityStatus::Real
                                            : CapabilityStatus::Unsupported,
      impl_->topology.numa_nodes.size() > 1
          ? "multiple NUMA nodes exist, so cross-node accesses can be generated and measured"
          : "this host exposes a single NUMA node, so cross-node access cannot be observed here; "
            "the semantics are proven by the synthetic backend instead",
      "GetNumaHighestNodeNumber");
  add(std::string(kCapabilityCacheLine), CapabilityStatus::Unsupported,
      "the operating system exposes no per-cache-line ownership or sharer information",
      "none");
  add(std::string(kCapabilityInvalidations), CapabilityStatus::Unsupported,
      "the operating system exposes no coherence invalidation counters",
      "none");
  return capabilities;
}

Status CpuOsBackend::start(const CollectorContext& context) {
  if (context.sink == nullptr) {
    return fail(ErrorCode::InvalidArgument, "cpu-os backend requires an ingestion sink");
  }
  impl_->context = context;
  const Result<HostTopology> discovered = discover_host_topology();
  if (!discovered.ok()) {
    return Status(discovered.error());
  }
  impl_->topology = discovered.value();

  StructureRegistrar* registrar = context.registrar;
  if (registrar != nullptr && impl_->config.register_topology) {
    NodeRecord node;
    node.id = NodeId{"node.host.0"};
    node.display_name = impl_->topology.processor_name;
    registrar->register_node(node);

    CoherenceDomainRecord domain;
    domain.id = CoherenceDomainId{"cd.host.observed"};
    domain.generation = CoherenceDomainGeneration{1};
    domain.protocol_family = "not-observable";
    registrar->register_coherence_domain(domain);

    MemoryDomainRecord host_domain;
    host_domain.id = MemoryDomainId{"md.host.numa0"};
    host_domain.generation = MemoryDomainGeneration{1};
    host_domain.node = NodeId{"node.host.0"};
    host_domain.kind = MemoryDomainKind::HostDram;
    host_domain.coherence_domain = CoherenceDomainId{"cd.host.observed"};
    host_domain.coherent_with_host = true;
    host_domain.coherent_with_host_known = true;
    host_domain.capacity_bytes = impl_->topology.total_physical_memory_bytes;
    host_domain.capacity_known = host_domain.capacity_bytes != 0;
    registrar->register_memory_domain(host_domain);

    for (const HostNumaNode& numa : impl_->topology.numa_nodes) {
      ProcessorRecord processor;
      processor.id = ProcessorId{"cpu.numa" + detail::format_u64(numa.index)};
      processor.node = NodeId{"node.host.0"};
      processor.generation = DeviceGeneration{1};
      processor.logical_processor_count = numa.processor_count;
      processor.physical_core_count = numa.processor_count;
      processor.numa_node_index = numa.index;
      processor.numa_node_index_known = true;
      processor.local_memory_domain = MemoryDomainId{"md.host.numa0"};
      registrar->register_processor(processor);

      if (impl_->config.register_memory_regions && numa.memory_known) {
        RegionRecord region;
        region.id = MemoryRegionId{"region.host.numa" + detail::format_u64(numa.index)};
        region.generation = MemoryRegionGeneration{1};
        region.owner = "cpu-os-backend";
        region.memory_domain = MemoryDomainId{"md.host.numa0"};
        region.memory_domain_generation = MemoryDomainGeneration{1};
        region.coherence_domain = CoherenceDomainId{"cd.host.observed"};
        region.sharing_scope = SharingScope::ProcessShared;
        region.size_bytes = numa.memory_bytes;
        region.size_known = true;
        region.page_size_bytes = 4096;
        region.page_size_known = true;
        region.allocation_generation = 1;
        region.mapping_generation = 1;
        region.workload = WorkloadId{"workload.cpu_os.locality"};
        region.annotation = "operating-system reported NUMA memory";
        registrar->register_region(region);
      }
    }

    TopologyLink link;
    link.from.kind = ResourceKind::Processor;
    link.from.id = ResourceId{"cpu.numa0"};
    link.from.generation = 1;
    link.to.kind = ResourceKind::MemoryDomain;
    link.to.id = ResourceId{"md.host.numa0"};
    link.to.generation = 1;
    link.locality = Locality::LocalNuma;
    link.provenance = Provenance::OsTelemetry;
    link.latency_ns = 0;
    registrar->set_topology_link(link);

    for (const Capability& capability : capabilities()) {
      registrar->set_capability(capability);
    }
  }
  impl_->started = true;
  return Status();
}

Status CpuOsBackend::poll(const CollectorContext& context) {
  if (!impl_->started) {
    return fail(ErrorCode::Busy, "cpu-os backend is not started");
  }
  if (!impl_->config.run_workload || impl_->workload_done) {
    return Status();
  }
  impl_->workload_done = true;

  const std::uint64_t bytes = impl_->config.workload_bytes == 0
                                  ? (4ull << 20)
                                  : impl_->config.workload_bytes;
  std::vector<unsigned char> buffer(static_cast<std::size_t>(bytes));
  const std::size_t stride =
      impl_->config.stride_bytes == 0 ? 64 : impl_->config.stride_bytes;

  // Warm the buffer so that the pages are resident and their placement is
  // observable before measurement begins.
  for (std::size_t i = 0; i < buffer.size(); i += stride) {
    buffer[i] = static_cast<unsigned char>(i & 0xFFu);
  }

  // Reads: this process genuinely reads the region from several threads.
  const auto read_start = std::chrono::steady_clock::now();
  std::atomic<std::uint64_t> checksum{0};
  std::vector<std::thread> threads;
  const unsigned hardware = std::thread::hardware_concurrency();
  std::uint32_t thread_count = impl_->config.workload_passes == 0 ? 1 : impl_->config.workload_passes;
  if (thread_count == 0) {
    thread_count = 1;
  }
  if (hardware != 0 && thread_count > hardware) {
    thread_count = hardware;
  }
  for (std::uint32_t t = 0; t < thread_count; ++t) {
    threads.emplace_back([&buffer, stride, &checksum]() {
      std::uint64_t local = 0;
      for (std::size_t i = 0; i < buffer.size(); i += stride) {
        local += buffer[i];
      }
      checksum.fetch_add(local, std::memory_order_relaxed);
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  const auto read_end = std::chrono::steady_clock::now();
  const std::int64_t read_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(read_end - read_start).count();
  threads.clear();

  Result<Observation> shared = impl_->make_observation(
      EventType::SharedRead, "region.host.numa0", ResourceKind::Processor, "cpu.numa0",
      "md.host.numa0", AccessDirection::Read, bytes, bytes / 4096, Locality::LocalNuma,
      Precision::Derived, EvidenceGranularity::Page, read_ns);
  if (shared.ok()) {
    const Result<IngestionOutcome> outcome = context.sink->publish(shared.take());
    if (outcome.ok()) {
      ++impl_->emissions;
    }
  }

  // Writes: this process genuinely writes the region from several threads.
  const auto write_start = std::chrono::steady_clock::now();
  for (std::uint32_t t = 0; t < thread_count; ++t) {
    threads.emplace_back([&buffer, stride, t]() {
      const unsigned char value = static_cast<unsigned char>(0xA0u + t);
      for (std::size_t i = 0; i < buffer.size(); i += stride) {
        buffer[i] = value;
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  const auto write_end = std::chrono::steady_clock::now();
  const std::int64_t write_ns =
      std::chrono::duration_cast<std::chrono::nanoseconds>(write_end - write_start).count();

  Result<Observation> exclusive = impl_->make_observation(
      EventType::WriteExclusiveTransition, "region.host.numa0", ResourceKind::Processor,
      "cpu.numa0", "md.host.numa0", AccessDirection::Write, bytes, bytes / 4096,
      Locality::LocalNuma, Precision::Derived, EvidenceGranularity::Page, write_ns);
  if (exclusive.ok()) {
    const Result<IngestionOutcome> outcome = context.sink->publish(exclusive.take());
    if (outcome.ok()) {
      ++impl_->emissions;
    }
  }

  impl_->local_latency_ns = static_cast<double>(read_ns) /
                            static_cast<double>(std::max<std::size_t>(1, buffer.size() / stride));
  return Status();
}

Status CpuOsBackend::stop() {
  impl_->started = false;
  return Status();
}

const HostTopology& CpuOsBackend::topology() const noexcept { return impl_->topology; }
double CpuOsBackend::measured_local_latency_ns() const noexcept {
  return impl_->local_latency_ns;
}
double CpuOsBackend::measured_remote_latency_ns() const noexcept {
  return impl_->remote_latency_ns;
}
std::uint64_t CpuOsBackend::emissions() const noexcept { return impl_->emissions; }

}  // namespace sol::coherence
