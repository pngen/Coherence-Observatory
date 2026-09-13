// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Shared fixtures for the test suites.

#ifndef COHERENCE_TESTS_TEST_SUPPORT_HPP
#define COHERENCE_TESTS_TEST_SUPPORT_HPP

#include <memory>
#include <string>
#include <vector>

#include "coherence/backend.hpp"
#include "coherence/observatory.hpp"

namespace cotest {

using sol::coherence::AcceleratorRecord;
using sol::coherence::CoherenceDomainId;
using sol::coherence::CoherenceDomainRecord;
using sol::coherence::DeviceGeneration;
using sol::coherence::Locality;
using sol::coherence::MemoryDomainGeneration;
using sol::coherence::MemoryDomainId;
using sol::coherence::MemoryDomainKind;
using sol::coherence::MemoryDomainRecord;
using sol::coherence::MemoryRegionGeneration;
using sol::coherence::MemoryRegionId;
using sol::coherence::ObserverId;
using sol::coherence::PublisherId;
using sol::coherence::RegionRecord;
using sol::coherence::ResourceId;
using sol::coherence::ResourceKind;
using sol::coherence::ResourceRef;
using sol::coherence::SharingScope;
using sol::coherence::TopologyLink;

inline sol::coherence::ObservatoryOptions test_options() {
  sol::coherence::ObservatoryOptions options;
  options.observer_id = ObserverId{"observer.test"};
  options.node_id = sol::coherence::NodeId{"node.test.0"};
  options.display_name = "test node";
  return options;
}

/// Registers a complete two-accelerator, two-domain topology.
inline void register_test_topology(sol::coherence::Observatory& observatory,
                                   std::size_t region_count = 4) {
  sol::coherence::NodeRecord node;
  node.id = sol::coherence::NodeId{"node.test.0"};
  node.display_name = "test node";
  observatory.register_node(node);

  CoherenceDomainRecord coherence_domain;
  coherence_domain.id = CoherenceDomainId{"cd.test"};
  coherence_domain.generation = sol::coherence::CoherenceDomainGeneration{1};
  coherence_domain.protocol_family = "synthetic-exact";
  observatory.register_coherence_domain(coherence_domain);

  auto add_domain = [&observatory](const char* id, MemoryDomainKind kind) {
    MemoryDomainRecord record;
    record.id = MemoryDomainId{id};
    record.generation = MemoryDomainGeneration{1};
    record.node = sol::coherence::NodeId{"node.test.0"};
    record.kind = kind;
    record.coherence_domain = CoherenceDomainId{"cd.test"};
    record.coherent_with_host = true;
    record.coherent_with_host_known = true;
    record.capacity_bytes = 1ull << 34;
    record.capacity_known = true;
    return observatory.register_memory_domain(record);
  };
  add_domain("md.host.a", MemoryDomainKind::HostDram);
  add_domain("md.host.b", MemoryDomainKind::HostDram);
  add_domain("md.acc.a", MemoryDomainKind::DeviceMemory);
  add_domain("md.acc.b", MemoryDomainKind::DeviceMemory);
  add_domain("md.cxl", MemoryDomainKind::CxlAttached);
  add_domain("md.pooled", MemoryDomainKind::PooledMemory);

  sol::coherence::ProcessorRecord processor;
  processor.id = sol::coherence::ProcessorId{"cpu.a"};
  processor.node = sol::coherence::NodeId{"node.test.0"};
  processor.generation = DeviceGeneration{1};
  processor.logical_processor_count = 8;
  processor.physical_core_count = 8;
  processor.numa_node_index = 0;
  processor.numa_node_index_known = true;
  processor.local_memory_domain = MemoryDomainId{"md.host.a"};
  observatory.register_processor(processor);

  sol::coherence::ProcessorRecord remote_processor;
  remote_processor.id = sol::coherence::ProcessorId{"cpu.b"};
  remote_processor.node = sol::coherence::NodeId{"node.test.0"};
  remote_processor.generation = DeviceGeneration{1};
  remote_processor.logical_processor_count = 8;
  remote_processor.physical_core_count = 8;
  remote_processor.numa_node_index = 1;
  remote_processor.numa_node_index_known = true;
  remote_processor.local_memory_domain = MemoryDomainId{"md.host.b"};
  observatory.register_processor(remote_processor);

  auto add_accelerator = [&observatory](const char* id, const char* domain) {
    AcceleratorRecord record;
    record.id = sol::coherence::AcceleratorId{id};
    record.node = sol::coherence::NodeId{"node.test.0"};
    record.generation = DeviceGeneration{1};
    record.vendor = "synthetic";
    record.vendor_uuid = std::string(id) + "-uuid";
    record.local_memory_domain = MemoryDomainId{domain};
    record.peer_coherent_capable = true;
    record.peer_coherent_capability_known = true;
    return observatory.register_accelerator(record);
  };
  add_accelerator("acc.a", "md.acc.a");
  add_accelerator("acc.b", "md.acc.b");

  for (std::size_t i = 0; i < region_count; ++i) {
    RegionRecord region;
    region.id = MemoryRegionId{"region.test." + std::to_string(i)};
    region.generation = MemoryRegionGeneration{1};
    region.owner = "test.workload";
    region.memory_domain = MemoryDomainId{i % 2 == 0 ? "md.host.a" : "md.host.b"};
    region.memory_domain_generation = MemoryDomainGeneration{1};
    region.coherence_domain = CoherenceDomainId{"cd.test"};
    region.sharing_scope = SharingScope::DeviceShared;
    region.size_bytes = 1u << 20;
    region.size_known = true;
    region.page_size_bytes = 4096;
    region.page_size_known = true;
    region.allocation_generation = 1;
    region.mapping_generation = 1;
    region.workload = sol::coherence::WorkloadId{"workload.test"};
    observatory.register_region(region);
  }

  auto link = [&observatory](ResourceKind from_kind, const char* from, ResourceKind to_kind,
                             const char* to, Locality locality) {
    TopologyLink topology_link;
    topology_link.from.kind = from_kind;
    topology_link.from.id = ResourceId{from};
    topology_link.from.generation = 1;
    topology_link.to.kind = to_kind;
    topology_link.to.id = ResourceId{to};
    topology_link.to.generation = 1;
    topology_link.locality = locality;
    topology_link.provenance = sol::coherence::Provenance::SyntheticBackend;
    observatory.set_topology_link(topology_link);
  };
  link(ResourceKind::Accelerator, "acc.a", ResourceKind::MemoryDomain, "md.host.a",
       Locality::LocalAccelerator);
  link(ResourceKind::Accelerator, "acc.b", ResourceKind::MemoryDomain, "md.host.a",
       Locality::PeerAccelerator);
  link(ResourceKind::Processor, "cpu.a", ResourceKind::MemoryDomain, "md.host.a",
       Locality::LocalNuma);
  link(ResourceKind::Processor, "cpu.b", ResourceKind::MemoryDomain, "md.host.a",
       Locality::RemoteNuma);
  link(ResourceKind::Processor, "cpu.a", ResourceKind::MemoryDomain, "md.cxl",
       Locality::CxlAttached);
  link(ResourceKind::Accelerator, "acc.a", ResourceKind::Accelerator, "acc.b",
       Locality::PeerAccelerator);
}

inline ResourceRef ref_accelerator(const char* id) {
  ResourceRef ref;
  ref.kind = ResourceKind::Accelerator;
  ref.id = ResourceId{id};
  ref.generation = 1;
  return ref;
}

inline ResourceRef ref_processor(const char* id) {
  ResourceRef ref;
  ref.kind = ResourceKind::Processor;
  ref.id = ResourceId{id};
  ref.generation = 1;
  return ref;
}

inline ResourceRef ref_domain(const char* id) {
  ResourceRef ref;
  ref.kind = ResourceKind::MemoryDomain;
  ref.id = ResourceId{id};
  ref.generation = 1;
  return ref;
}

/// Deterministic, non-zero boot identity derived from a publisher name.
///
/// Tests use these so that state fingerprints are reproducible; production
/// publishers use make_publisher_boot_id().
inline sol::coherence::PublisherBootId deterministic_boot(const char* publisher) noexcept {
  std::uint64_t hash = 1469598103934665603ull;
  for (const char* c = publisher; *c != '\0'; ++c) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(*c));
    hash *= 1099511628211ull;
  }
  return sol::coherence::PublisherBootId{hash == 0 ? 1 : hash};
}

/// Starts an in-process publisher host with a deterministic boot identity.
inline std::unique_ptr<sol::coherence::LocalPublisherHost> start_host(
    sol::coherence::Observatory& observatory, const char* publisher = "pub.test.1",
    sol::coherence::Provenance provenance = sol::coherence::Provenance::SyntheticBackend) {
  sol::coherence::PublisherRegistration registration;
  registration.id = PublisherId{publisher};
  registration.boot = deterministic_boot(publisher);
  registration.observer = ObserverId{"observer.test"};
  registration.node = sol::coherence::NodeId{"node.test.0"};
  registration.display_name = publisher;
  registration.provenance = provenance;
  auto host = std::make_unique<sol::coherence::LocalPublisherHost>(observatory, registration);
  host->start();
  return host;
}

/// Fills the identity fields a hand-built observation needs before it can be
/// handed to Observatory::ingest or Observatory::attribute directly.
inline sol::coherence::Observation authorized(sol::coherence::Observatory& observatory,
                                              const char* publisher, std::uint64_t sequence,
                                              sol::coherence::Observation observation) {
  observation.source_publisher = sol::coherence::PublisherId{publisher};
  observation.publisher_boot = deterministic_boot(publisher);
  observation.event_id = sol::coherence::CoherenceEventId{sequence};
  observation.sequence = sol::coherence::EventSequence{sequence};
  observation.coordinator_epoch = observatory.coordinator_epoch();
  observation.topology_generation = observatory.topology_generation();
  return observation;
}

/// Builds an exact per-region observation.
inline sol::coherence::Observation exact_observation(std::size_t region_index,
                                                sol::coherence::EventType type,
                                                sol::coherence::Nanos timestamp) {
  sol::coherence::Observation observation;
  observation.type = type;
  observation.timestamp_ns = timestamp;
  observation.direction = sol::coherence::AccessDirection::Read;
  observation.bytes = 64;
  observation.lines = 1;
  observation.source = ref_accelerator("acc.a");
  observation.target = ref_domain("md.host.a");
  observation.region = MemoryRegionId{"region.test." + std::to_string(region_index)};
  observation.region_generation = MemoryRegionGeneration{1};
  observation.precision = sol::coherence::Precision::ExactEvent;
  observation.granularity = sol::coherence::EvidenceGranularity::Region;
  observation.provenance = sol::coherence::Provenance::SyntheticBackend;
  return observation;
}

}  // namespace cotest

#endif  // COHERENCE_TESTS_TEST_SUPPORT_HPP