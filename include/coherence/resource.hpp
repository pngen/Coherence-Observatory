// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Registered resources: nodes, processors, accelerators, memory domains and
// coherence domains.
//
// Coherence Observatory consumes identities and topology from adjacent
// runtimes; it does not own placement, allocation, scheduling or protocol
// state.  A registration here is a statement about what exists and which
// generation of it the evidence refers to.

#ifndef COHERENCE_RESOURCE_HPP
#define COHERENCE_RESOURCE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "coherence/ids.hpp"
#include "coherence/taxonomy.hpp"
#include "coherence/time.hpp"

namespace sol::coherence {

/// A reference to a registered resource at a specific generation.
struct COHERENCE_API ResourceRef {
  ResourceKind kind = ResourceKind::Unknown;
  ResourceId id;
  std::uint64_t generation = 0;

  bool empty() const noexcept { return id.empty(); }

  friend bool operator==(const ResourceRef&, const ResourceRef&) = default;
};

/// Kind of memory a domain provides.
enum class MemoryDomainKind : std::uint8_t {
  Unknown = 0,
  HostDram = 1,
  DeviceMemory = 2,
  HighBandwidthMemory = 3,
  CxlAttached = 4,
  PooledMemory = 5,
  RemoteNodeMemory = 6,
};

COHERENCE_API std::string_view to_string(MemoryDomainKind kind) noexcept;
COHERENCE_API Result<MemoryDomainKind> parse_memory_domain_kind(std::string_view text);

/// Node: a machine or coherent node boundary.
struct COHERENCE_API NodeRecord {
  NodeId id;
  std::string display_name;
  Nanos registered_at_ns = 0;
};

/// Processor: a CPU complex / socket / NUMA-local processor group.
struct COHERENCE_API ProcessorRecord {
  ProcessorId id;
  NodeId node;
  DeviceGeneration generation;
  std::uint32_t logical_processor_count = 0;
  std::uint32_t physical_core_count = 0;
  std::uint32_t numa_node_index = 0;
  bool numa_node_index_known = false;
  MemoryDomainId local_memory_domain;
  bool retired = false;
  Nanos registered_at_ns = 0;
};

/// Accelerator: a GPU / FPGA / offload device.
struct COHERENCE_API AcceleratorRecord {
  AcceleratorId id;
  NodeId node;
  DeviceGeneration generation;
  std::string vendor;
  /// Vendor-stable device identity when the vendor exposes one (e.g. NVML UUID).
  std::string vendor_uuid;
  MemoryDomainId local_memory_domain;
  bool peer_coherent_capable = false;
  bool peer_coherent_capability_known = false;
  bool retired = false;
  Nanos registered_at_ns = 0;
};

/// Memory domain: an address space with its own coherence and cost behaviour.
struct COHERENCE_API MemoryDomainRecord {
  MemoryDomainId id;
  MemoryDomainGeneration generation;
  NodeId node;
  MemoryDomainKind kind = MemoryDomainKind::Unknown;
  CoherenceDomainId coherence_domain;
  bool coherent_with_host = false;
  bool coherent_with_host_known = false;
  std::uint64_t capacity_bytes = 0;
  bool capacity_known = false;
  bool retired = false;
  Nanos registered_at_ns = 0;
};

/// Coherence domain: a set of resources among which coherence is maintained
/// by a single protocol instance.
struct COHERENCE_API CoherenceDomainRecord {
  CoherenceDomainId id;
  CoherenceDomainGeneration generation;
  /// Protocol family as reported by the source ("mesi", "chi", "nvlink-c2c",
  /// "none-observable", ...).  Purely descriptive: the observatory does not
  /// implement or interpret the protocol.
  std::string protocol_family;
  std::vector<ResourceRef> members;
  bool retired = false;
  Nanos registered_at_ns = 0;
};

/// Structural state loaded from durable storage is never live on its own.
struct COHERENCE_API LoadProvenance {
  bool loaded_from_state = false;
  std::uint64_t source_coordinator_epoch = 0;
};

}  // namespace sol::coherence

#endif  // COHERENCE_RESOURCE_HPP
