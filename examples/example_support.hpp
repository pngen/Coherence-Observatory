// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Shared example scaffolding.  Examples use only the public API.

#ifndef COHERENCE_EXAMPLES_SUPPORT_HPP
#define COHERENCE_EXAMPLES_SUPPORT_HPP

#include <cstdio>
#include <iostream>
#include <string>

#include "coherence/backend.hpp"
#include "coherence/explanation.hpp"
#include "coherence/observatory.hpp"
#include "coherence/version.hpp"

namespace example {

inline std::string bool_text(bool value) { return value ? "true" : "false"; }

inline sol::coherence::ObservatoryOptions default_options() {
  sol::coherence::ObservatoryOptions options;
  options.observer_id = sol::coherence::ObserverId{"observer.example"};
  options.node_id = sol::coherence::NodeId{"node.example.0"};
  options.display_name = "example node";
  return options;
}

/// Registers a small but complete synthetic topology through the public API.
inline void register_topology(sol::coherence::Observatory& observatory) {
  sol::coherence::NodeRecord node;
  node.id = sol::coherence::NodeId{"node.example.0"};
  node.display_name = "example node";
  observatory.register_node(node);

  sol::coherence::CoherenceDomainRecord coherence_domain;
  coherence_domain.id = sol::coherence::CoherenceDomainId{"cd.example"};
  coherence_domain.generation = sol::coherence::CoherenceDomainGeneration{1};
  coherence_domain.protocol_family = "synthetic-exact";
  observatory.register_coherence_domain(coherence_domain);

  auto domain = [&observatory](const char* id, sol::coherence::MemoryDomainKind kind) {
    sol::coherence::MemoryDomainRecord record;
    record.id = sol::coherence::MemoryDomainId{id};
    record.generation = sol::coherence::MemoryDomainGeneration{1};
    record.node = sol::coherence::NodeId{"node.example.0"};
    record.kind = kind;
    record.coherence_domain = sol::coherence::CoherenceDomainId{"cd.example"};
    record.coherent_with_host = true;
    record.coherent_with_host_known = true;
    record.capacity_bytes = 1ull << 34;
    record.capacity_known = true;
    observatory.register_memory_domain(record);
  };
  domain("md.host", sol::coherence::MemoryDomainKind::HostDram);
  domain("md.acc.a", sol::coherence::MemoryDomainKind::DeviceMemory);
  domain("md.acc.b", sol::coherence::MemoryDomainKind::DeviceMemory);
  domain("md.cxl", sol::coherence::MemoryDomainKind::CxlAttached);

  sol::coherence::ProcessorRecord processor;
  processor.id = sol::coherence::ProcessorId{"cpu.0"};
  processor.node = sol::coherence::NodeId{"node.example.0"};
  processor.generation = sol::coherence::DeviceGeneration{1};
  processor.logical_processor_count = 8;
  processor.physical_core_count = 8;
  processor.numa_node_index = 0;
  processor.numa_node_index_known = true;
  processor.local_memory_domain = sol::coherence::MemoryDomainId{"md.host"};
  observatory.register_processor(processor);

  auto accelerator = [&observatory](const char* id, const char* local_domain) {
    sol::coherence::AcceleratorRecord record;
    record.id = sol::coherence::AcceleratorId{id};
    record.node = sol::coherence::NodeId{"node.example.0"};
    record.generation = sol::coherence::DeviceGeneration{1};
    record.vendor = "synthetic";
    record.vendor_uuid = std::string(id) + "-uuid";
    record.local_memory_domain = sol::coherence::MemoryDomainId{local_domain};
    record.peer_coherent_capable = true;
    record.peer_coherent_capability_known = true;
    observatory.register_accelerator(record);
  };
  accelerator("acc.a", "md.acc.a");
  accelerator("acc.b", "md.acc.b");

  auto region = [&observatory](const char* id, const char* memory_domain, std::uint64_t size) {
    sol::coherence::RegionRecord record;
    record.id = sol::coherence::MemoryRegionId{id};
    record.generation = sol::coherence::MemoryRegionGeneration{1};
    record.owner = "example.workload";
    record.memory_domain = sol::coherence::MemoryDomainId{memory_domain};
    record.memory_domain_generation = sol::coherence::MemoryDomainGeneration{1};
    record.coherence_domain = sol::coherence::CoherenceDomainId{"cd.example"};
    record.sharing_scope = sol::coherence::SharingScope::DeviceShared;
    record.size_bytes = size;
    record.size_known = true;
    record.page_size_bytes = 4096;
    record.page_size_known = true;
    record.allocation_generation = 1;
    record.mapping_generation = 1;
    record.workload = sol::coherence::WorkloadId{"workload.example"};
    observatory.register_region(record);
  };
  region("region.shared", "md.host", 1u << 20);
  region("region.pingpong", "md.acc.a", 4096);
  region("region.line", "md.host", 4096);
  region("region.cxl", "md.cxl", 1u << 20);

  auto link = [&observatory](sol::coherence::ResourceKind from_kind, const char* from,
                             sol::coherence::ResourceKind to_kind, const char* to,
                             sol::coherence::Locality locality) {
    sol::coherence::TopologyLink topology_link;
    topology_link.from.kind = from_kind;
    topology_link.from.id = sol::coherence::ResourceId{from};
    topology_link.from.generation = 1;
    topology_link.to.kind = to_kind;
    topology_link.to.id = sol::coherence::ResourceId{to};
    topology_link.to.generation = 1;
    topology_link.locality = locality;
    topology_link.provenance = sol::coherence::Provenance::SyntheticBackend;
    observatory.set_topology_link(topology_link);
  };
  using sol::coherence::Locality;
  using sol::coherence::ResourceKind;
  link(ResourceKind::Accelerator, "acc.a", ResourceKind::MemoryDomain, "md.host",
       Locality::LocalAccelerator);
  link(ResourceKind::Accelerator, "acc.b", ResourceKind::MemoryDomain, "md.host",
       Locality::PeerAccelerator);
  link(ResourceKind::Processor, "cpu.0", ResourceKind::MemoryDomain, "md.host",
       Locality::LocalNuma);
  link(ResourceKind::Processor, "cpu.0", ResourceKind::MemoryDomain, "md.cxl",
       Locality::CxlAttached);
  link(ResourceKind::Accelerator, "acc.a", ResourceKind::Accelerator, "acc.b",
       Locality::PeerAccelerator);
}

/// Starts an in-process publisher host on \p observatory.
inline sol::coherence::Status start_host(sol::coherence::Observatory& observatory,
                                    sol::coherence::LocalPublisherHost** host_out,
                                    std::unique_ptr<sol::coherence::LocalPublisherHost>* storage,
                                    const char* publisher_id = "pub.example.1") {
  sol::coherence::PublisherRegistration registration;
  registration.id = sol::coherence::PublisherId{publisher_id};
  registration.boot = sol::coherence::make_publisher_boot_id();
  registration.observer = sol::coherence::ObserverId{"observer.example"};
  registration.node = sol::coherence::NodeId{"node.example.0"};
  registration.display_name = publisher_id;
  registration.provenance = sol::coherence::Provenance::SyntheticBackend;
  *storage =
      std::make_unique<sol::coherence::LocalPublisherHost>(observatory, registration);
  const sol::coherence::Status started = (*storage)->start();
  if (host_out != nullptr) {
    *host_out = storage->get();
  }
  return started;
}

inline sol::coherence::ResourceRef accelerator_ref(const char* id) {
  sol::coherence::ResourceRef ref;
  ref.kind = sol::coherence::ResourceKind::Accelerator;
  ref.id = sol::coherence::ResourceId{id};
  ref.generation = 1;
  return ref;
}

inline sol::coherence::ResourceRef processor_ref(const char* id) {
  sol::coherence::ResourceRef ref;
  ref.kind = sol::coherence::ResourceKind::Processor;
  ref.id = sol::coherence::ResourceId{id};
  ref.generation = 1;
  return ref;
}

inline sol::coherence::ResourceRef domain_ref(const char* id) {
  sol::coherence::ResourceRef ref;
  ref.kind = sol::coherence::ResourceKind::MemoryDomain;
  ref.id = sol::coherence::ResourceId{id};
  ref.generation = 1;
  return ref;
}

inline void print_findings(const sol::coherence::Observatory& observatory) {
  const sol::coherence::Result<std::vector<sol::coherence::Finding>> findings =
      observatory.findings();
  if (!findings.ok()) {
    std::cout << "findings unavailable: " << findings.describe() << "\n";
    return;
  }
  if (findings.value().empty()) {
    std::cout << "(no findings)\n";
    return;
  }
  for (const sol::coherence::Finding& finding : findings.value()) {
    std::cout << sol::coherence::render_text(sol::coherence::explain(finding));
  }
}

}  // namespace example

#endif  // COHERENCE_EXAMPLES_SUPPORT_HPP