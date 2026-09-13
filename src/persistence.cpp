// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Versioned, integrity-checked durable structural state.
//
// File layout (all fields little-endian):
//
//   Header (64 bytes)
//     0  char[8]  magic "COBSST01"
//     8  u32      version
//     12 u32      flags
//     16 u64      payload_length
//     24 u64      coordinator_epoch
//     32 u64      observation_epoch
//     40 u64      record_count
//     48 u32      header_crc   (CRC-32 of the header with this field zeroed)
//     52 u32      reserved
//     56 u64      reserved
//   Payload
//     repeated: SectionHeader (16 bytes) { u32 type, u32 flags, u64 length }
//               body (length - 4 bytes)
//               u32 body_crc
//   Trailer (16 bytes)
//     0  char[8]  magic "COBSEND1"
//     8  u32      payload_crc
//     12 u32      reserved
//
// Loading parses the entire file into staging storage and validates every
// relationship before the live state is touched, so a corrupt file can never
// partially apply.

#include "detail/persistence.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "coherence/protocol.hpp"
#include "detail/platform.hpp"
#include "text_util.hpp"

namespace sol::coherence {
namespace detail {
namespace {

constexpr char kStateMagic[8] = {'C', 'O', 'B', 'S', 'S', 'T', '0', '1'};
constexpr char kStateTrailerMagic[8] = {'C', 'O', 'B', 'S', 'E', 'N', 'D', '1'};

enum class SectionType : std::uint32_t {
  Meta = 1,
  Nodes = 2,
  Processors = 3,
  Accelerators = 4,
  MemoryDomains = 5,
  CoherenceDomains = 6,
  Regions = 7,
  Topology = 8,
  PublisherWatermarks = 9,
  Capabilities = 10,
  CostModel = 11,
  HistoricalAggregates = 12,
  History = 13,
};

void put_u32(std::uint8_t* out, std::uint32_t value) noexcept {
  for (int i = 0; i < 4; ++i) {
    out[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

void put_u64(std::uint8_t* out, std::uint64_t value) noexcept {
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

std::uint32_t get_u32(const std::uint8_t* in) noexcept {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(in[i]) << (8 * i);
  }
  return value;
}

std::uint64_t get_u64(const std::uint8_t* in) noexcept {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(in[i]) << (8 * i);
  }
  return value;
}

Status write_text(PayloadWriter& writer, const std::string& value, std::size_t max_length) {
  return writer.text(value, max_length);
}

Status read_text(PayloadReader& reader, std::string* value, std::size_t max_length) {
  return reader.text(value, max_length);
}

Status read_u32(PayloadReader& reader, std::uint32_t* value) { return reader.u32(value); }
Status read_u64(PayloadReader& reader, std::uint64_t* value) { return reader.u64(value); }

std::vector<std::uint8_t> build_section(SectionType type, const std::vector<std::uint8_t>& body) {
  std::vector<std::uint8_t> out;
  out.resize(Limits::kStateSectionHeaderSize + body.size() + 4);
  put_u32(out.data(), static_cast<std::uint32_t>(type));
  put_u32(out.data() + 4, 0);
  put_u64(out.data() + 8, static_cast<std::uint64_t>(body.size()) + 4);
  if (!body.empty()) {
    std::memcpy(out.data() + Limits::kStateSectionHeaderSize, body.data(), body.size());
  }
  put_u32(out.data() + Limits::kStateSectionHeaderSize + body.size(),
          crc32(body.data(), body.size()));
  return out;
}

/// Staging storage: every field is validated before any of it is applied.
struct StagedState {
  bool has_meta = false;
  ObserverId observer_id;
  NodeId node_id;
  std::string display_name;
  TopologyGeneration topology_generation;
  EvidenceGeneration evidence_generation;
  std::int64_t time_bucket_ns = 1000000000;
  CoordinatorEpoch source_coordinator_epoch;
  ObservationEpoch source_observation_epoch;

  std::map<std::string, NodeRecord> nodes;
  std::map<std::string, ProcessorRecord> processors;
  std::map<std::string, AcceleratorRecord> accelerators;
  std::map<std::string, MemoryDomainRecord> memory_domains;
  std::map<std::string, CoherenceDomainRecord> coherence_domains;
  std::map<std::string, RegionRecord> regions;
  std::vector<TopologyLink> links;
  std::map<std::string, PublisherWatermark> watermarks;
  std::map<std::string, Capability> capabilities;
  CostModel cost_model;
  bool has_cost_model = false;
  std::map<AggregateKey, AggregateValue> historical_aggregates;
  std::vector<HistoricalEvidence> history;
  std::set<std::uint32_t> seen_sections;
  std::uint64_t record_count = 0;
};

Status stage_meta(PayloadReader& reader, StagedState& staged) {
  std::uint32_t schema_version = 0;
  Status status = read_u32(reader, &schema_version);
  if (!status.ok()) return status;
  if (schema_version != 1) {
    return fail(ErrorCode::UnsupportedStateVersion, "unsupported state schema version",
                format_u64(schema_version));
  }
  std::string observer;
  status = read_text(reader, &observer, Limits::kMaxNameLength);
  if (!status.ok()) return status;
  std::string node;
  status = read_text(reader, &node, Limits::kMaxNameLength);
  if (!status.ok()) return status;
  std::string display;
  status = read_text(reader, &display, Limits::kMaxRegionAnnotationLength);
  if (!status.ok()) return status;
  std::uint64_t topology = 0;
  status = read_u64(reader, &topology);
  if (!status.ok()) return status;
  std::uint64_t evidence = 0;
  status = read_u64(reader, &evidence);
  if (!status.ok()) return status;
  std::uint64_t bucket = 0;
  status = read_u64(reader, &bucket);
  if (!status.ok()) return status;
  std::uint64_t coordinator = 0;
  status = read_u64(reader, &coordinator);
  if (!status.ok()) return status;
  std::uint64_t observation = 0;
  status = read_u64(reader, &observation);
  if (!status.ok()) return status;

  if (!observer.empty()) {
    const Result<ObserverId> parsed = ObserverId::parse(observer);
    if (!parsed.ok()) return Status(parsed.error());
    staged.observer_id = parsed.value();
  }
  if (!node.empty()) {
    const Result<NodeId> parsed = NodeId::parse(node);
    if (!parsed.ok()) return Status(parsed.error());
    staged.node_id = parsed.value();
  }
  staged.display_name = display;
  staged.topology_generation = TopologyGeneration{topology};
  staged.evidence_generation = EvidenceGeneration{evidence};
  if (bucket > static_cast<std::uint64_t>(Limits::kMaxTimestampNs)) {
    return fail(ErrorCode::CorruptState, "time bucket width out of range");
  }
  staged.time_bucket_ns = static_cast<std::int64_t>(bucket);
  staged.source_coordinator_epoch = CoordinatorEpoch{coordinator};
  staged.source_observation_epoch = ObservationEpoch{observation};
  staged.has_meta = true;
  return reader.require_exhausted();
}

Status stage_nodes(PayloadReader& reader, StagedState& staged) {
  std::uint64_t count = 0;
  Status status = read_u64(reader, &count);
  if (!status.ok()) return status;
  if (count > Limits::kMaxNodeCount) {
    return fail(ErrorCode::CorruptState, "node count exceeds the bound");
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    NodeRecord record;
    std::string id;
    status = read_text(reader, &id, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    status = read_text(reader, &record.display_name, Limits::kMaxRegionAnnotationLength);
    if (!status.ok()) return status;
    std::uint64_t registered = 0;
    status = read_u64(reader, &registered);
    if (!status.ok()) return status;
    const Result<NodeId> parsed = NodeId::parse(id);
    if (!parsed.ok()) {
      return fail(ErrorCode::CorruptState, "invalid node identity", id);
    }
    record.id = parsed.value();
    record.registered_at_ns = static_cast<Nanos>(registered);
    if (staged.nodes.find(id) != staged.nodes.end()) {
      return fail(ErrorCode::CorruptState, "duplicate node identity", id);
    }
    staged.nodes[id] = std::move(record);
  }
  return reader.require_exhausted();
}

Status stage_processors(PayloadReader& reader, StagedState& staged) {
  std::uint64_t count = 0;
  Status status = read_u64(reader, &count);
  if (!status.ok()) return status;
  if (count > Limits::kMaxProcessorCount) {
    return fail(ErrorCode::CorruptState, "processor count exceeds the bound");
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    ProcessorRecord record;
    std::string id;
    status = read_text(reader, &id, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    std::string node;
    status = read_text(reader, &node, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    std::string local_domain;
    status = read_text(reader, &local_domain, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    std::uint64_t generation = 0;
    status = read_u64(reader, &generation);
    if (!status.ok()) return status;
    std::uint32_t logical = 0;
    status = read_u32(reader, &logical);
    if (!status.ok()) return status;
    std::uint32_t physical = 0;
    status = read_u32(reader, &physical);
    if (!status.ok()) return status;
    std::uint32_t numa = 0;
    status = read_u32(reader, &numa);
    if (!status.ok()) return status;
    std::uint8_t numa_known = 0;
    status = reader.u8(&numa_known);
    if (!status.ok()) return status;
    std::uint8_t retired = 0;
    status = reader.u8(&retired);
    if (!status.ok()) return status;
    std::uint64_t registered = 0;
    status = read_u64(reader, &registered);
    if (!status.ok()) return status;

    const Result<ProcessorId> parsed = ProcessorId::parse(id);
    if (!parsed.ok()) {
      return fail(ErrorCode::CorruptState, "invalid processor identity", id);
    }
    record.id = parsed.value();
    record.node = NodeId{node};
    record.local_memory_domain = MemoryDomainId{local_domain};
    if (generation == 0) {
      return fail(ErrorCode::CorruptState, "processor generation must be non-zero", id);
    }
    record.generation = DeviceGeneration{generation};
    record.logical_processor_count = logical;
    record.physical_core_count = physical;
    record.numa_node_index = numa;
    record.numa_node_index_known = numa_known != 0;
    record.retired = retired != 0;
    record.registered_at_ns = static_cast<Nanos>(registered);
    if (staged.processors.find(id) != staged.processors.end()) {
      return fail(ErrorCode::CorruptState, "duplicate processor identity", id);
    }
    staged.processors[id] = std::move(record);
  }
  return reader.require_exhausted();
}

Status stage_accelerators(PayloadReader& reader, StagedState& staged) {
  std::uint64_t count = 0;
  Status status = read_u64(reader, &count);
  if (!status.ok()) return status;
  if (count > Limits::kMaxAcceleratorCount) {
    return fail(ErrorCode::CorruptState, "accelerator count exceeds the bound");
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    AcceleratorRecord record;
    std::string id;
    status = read_text(reader, &id, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    std::string node;
    status = read_text(reader, &node, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    status = read_text(reader, &record.vendor, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    status = read_text(reader, &record.vendor_uuid, Limits::kMaxRegionAnnotationLength);
    if (!status.ok()) return status;
    std::string local_domain;
    status = read_text(reader, &local_domain, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    std::uint64_t generation = 0;
    status = read_u64(reader, &generation);
    if (!status.ok()) return status;
    std::uint8_t peer_capable = 0;
    status = reader.u8(&peer_capable);
    if (!status.ok()) return status;
    std::uint8_t peer_known = 0;
    status = reader.u8(&peer_known);
    if (!status.ok()) return status;
    std::uint8_t retired = 0;
    status = reader.u8(&retired);
    if (!status.ok()) return status;
    std::uint64_t registered = 0;
    status = read_u64(reader, &registered);
    if (!status.ok()) return status;

    const Result<AcceleratorId> parsed = AcceleratorId::parse(id);
    if (!parsed.ok()) {
      return fail(ErrorCode::CorruptState, "invalid accelerator identity", id);
    }
    record.id = parsed.value();
    record.node = NodeId{node};
    record.local_memory_domain = MemoryDomainId{local_domain};
    if (generation == 0) {
      return fail(ErrorCode::CorruptState, "accelerator generation must be non-zero", id);
    }
    record.generation = DeviceGeneration{generation};
    record.peer_coherent_capable = peer_capable != 0;
    record.peer_coherent_capability_known = peer_known != 0;
    record.retired = retired != 0;
    record.registered_at_ns = static_cast<Nanos>(registered);
    if (staged.accelerators.find(id) != staged.accelerators.end()) {
      return fail(ErrorCode::CorruptState, "duplicate accelerator identity", id);
    }
    staged.accelerators[id] = std::move(record);
  }
  return reader.require_exhausted();
}

Status stage_memory_domains(PayloadReader& reader, StagedState& staged) {
  std::uint64_t count = 0;
  Status status = read_u64(reader, &count);
  if (!status.ok()) return status;
  if (count > Limits::kMaxMemoryDomainCount) {
    return fail(ErrorCode::CorruptState, "memory domain count exceeds the bound");
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    MemoryDomainRecord record;
    std::string id;
    status = read_text(reader, &id, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    std::string node;
    status = read_text(reader, &node, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    std::string coherence;
    status = read_text(reader, &coherence, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    std::uint64_t generation = 0;
    status = read_u64(reader, &generation);
    if (!status.ok()) return status;
    std::uint8_t kind = 0;
    status = reader.u8(&kind);
    if (!status.ok()) return status;
    std::uint8_t coherent = 0;
    status = reader.u8(&coherent);
    if (!status.ok()) return status;
    std::uint8_t coherent_known = 0;
    status = reader.u8(&coherent_known);
    if (!status.ok()) return status;
    std::uint8_t capacity_known = 0;
    status = reader.u8(&capacity_known);
    if (!status.ok()) return status;
    std::uint8_t retired = 0;
    status = reader.u8(&retired);
    if (!status.ok()) return status;
    std::uint64_t capacity = 0;
    status = read_u64(reader, &capacity);
    if (!status.ok()) return status;
    std::uint64_t registered = 0;
    status = read_u64(reader, &registered);
    if (!status.ok()) return status;

    if (kind > 6) {
      return fail(ErrorCode::CorruptState, "memory domain kind out of range", id);
    }
    const Result<MemoryDomainId> parsed = MemoryDomainId::parse(id);
    if (!parsed.ok()) {
      return fail(ErrorCode::CorruptState, "invalid memory domain identity", id);
    }
    record.id = parsed.value();
    record.node = NodeId{node};
    record.coherence_domain = CoherenceDomainId{coherence};
    if (generation == 0) {
      return fail(ErrorCode::CorruptState, "memory domain generation must be non-zero", id);
    }
    record.generation = MemoryDomainGeneration{generation};
    record.kind = static_cast<MemoryDomainKind>(kind);
    record.coherent_with_host = coherent != 0;
    record.coherent_with_host_known = coherent_known != 0;
    record.capacity_known = capacity_known != 0;
    record.retired = retired != 0;
    record.capacity_bytes = capacity;
    record.registered_at_ns = static_cast<Nanos>(registered);
    if (staged.memory_domains.find(id) != staged.memory_domains.end()) {
      return fail(ErrorCode::CorruptState, "duplicate memory domain identity", id);
    }
    staged.memory_domains[id] = std::move(record);
  }
  return reader.require_exhausted();
}

Status stage_coherence_domains(PayloadReader& reader, StagedState& staged) {
  std::uint64_t count = 0;
  Status status = read_u64(reader, &count);
  if (!status.ok()) return status;
  if (count > Limits::kMaxCoherenceDomainCount) {
    return fail(ErrorCode::CorruptState, "coherence domain count exceeds the bound");
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    CoherenceDomainRecord record;
    std::string id;
    status = read_text(reader, &id, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    status = read_text(reader, &record.protocol_family, Limits::kMaxRegionAnnotationLength);
    if (!status.ok()) return status;
    std::uint64_t generation = 0;
    status = read_u64(reader, &generation);
    if (!status.ok()) return status;
    std::uint8_t retired = 0;
    status = reader.u8(&retired);
    if (!status.ok()) return status;
    std::uint64_t member_count = 0;
    status = read_u64(reader, &member_count);
    if (!status.ok()) return status;
    if (member_count > Limits::kMaxTopologyLinkCount) {
      return fail(ErrorCode::CorruptState, "coherence domain member count exceeds the bound", id);
    }
    std::vector<ResourceRef> members;
    members.reserve(static_cast<std::size_t>(member_count));
    for (std::uint64_t m = 0; m < member_count; ++m) {
      ResourceRef member;
      std::uint8_t kind = 0;
      status = reader.u8(&kind);
      if (!status.ok()) return status;
      std::string member_id;
      status = read_text(reader, &member_id, Limits::kMaxNameLength);
      if (!status.ok()) return status;
      std::uint64_t member_generation = 0;
      status = read_u64(reader, &member_generation);
      if (!status.ok()) return status;
      if (kind > 5) {
        return fail(ErrorCode::CorruptState, "coherence domain member kind out of range", id);
      }
      member.kind = static_cast<ResourceKind>(kind);
      member.id = ResourceId{member_id};
      member.generation = member_generation;
      members.push_back(std::move(member));
    }
    std::uint64_t registered = 0;
    status = read_u64(reader, &registered);
    if (!status.ok()) return status;

    const Result<CoherenceDomainId> parsed = CoherenceDomainId::parse(id);
    if (!parsed.ok()) {
      return fail(ErrorCode::CorruptState, "invalid coherence domain identity", id);
    }
    record.id = parsed.value();
    if (generation == 0) {
      return fail(ErrorCode::CorruptState, "coherence domain generation must be non-zero", id);
    }
    record.generation = CoherenceDomainGeneration{generation};
    record.retired = retired != 0;
    record.members = std::move(members);
    record.registered_at_ns = static_cast<Nanos>(registered);
    if (staged.coherence_domains.find(id) != staged.coherence_domains.end()) {
      return fail(ErrorCode::CorruptState, "duplicate coherence domain identity", id);
    }
    staged.coherence_domains[id] = std::move(record);
  }
  return reader.require_exhausted();
}

Status stage_regions(PayloadReader& reader, StagedState& staged) {
  std::uint64_t count = 0;
  Status status = read_u64(reader, &count);
  if (!status.ok()) return status;
  if (count > Limits::kMaxRegionCount) {
    return fail(ErrorCode::CorruptState, "region count exceeds the bound");
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    RegionRecord record;
    std::string id;
    status = read_text(reader, &id, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    status = read_text(reader, &record.owner, Limits::kMaxRegionAnnotationLength);
    if (!status.ok()) return status;
    status = read_text(reader, &record.annotation, Limits::kMaxRegionAnnotationLength);
    if (!status.ok()) return status;
    std::string domain;
    status = read_text(reader, &domain, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    std::string coherence;
    status = read_text(reader, &coherence, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    std::string workload;
    status = read_text(reader, &workload, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    std::uint64_t generation = 0;
    status = read_u64(reader, &generation);
    if (!status.ok()) return status;
    std::uint64_t domain_generation = 0;
    status = read_u64(reader, &domain_generation);
    if (!status.ok()) return status;
    std::uint64_t handle = 0;
    status = read_u64(reader, &handle);
    if (!status.ok()) return status;
    std::uint64_t size = 0;
    status = read_u64(reader, &size);
    if (!status.ok()) return status;
    std::uint64_t page_size = 0;
    status = read_u64(reader, &page_size);
    if (!status.ok()) return status;
    std::uint64_t allocation_generation = 0;
    status = read_u64(reader, &allocation_generation);
    if (!status.ok()) return status;
    std::uint64_t mapping_generation = 0;
    status = read_u64(reader, &mapping_generation);
    if (!status.ok()) return status;
    std::uint8_t scope = 0;
    status = reader.u8(&scope);
    if (!status.ok()) return status;
    std::uint8_t size_known = 0;
    status = reader.u8(&size_known);
    if (!status.ok()) return status;
    std::uint8_t page_known = 0;
    status = reader.u8(&page_known);
    if (!status.ok()) return status;
    std::uint8_t retired = 0;
    status = reader.u8(&retired);
    if (!status.ok()) return status;
    std::uint64_t registered = 0;
    status = read_u64(reader, &registered);
    if (!status.ok()) return status;
    std::uint64_t retired_at = 0;
    status = read_u64(reader, &retired_at);
    if (!status.ok()) return status;

    if (scope > 5) {
      return fail(ErrorCode::CorruptState, "region sharing scope out of range", id);
    }
    const Result<MemoryRegionId> parsed = MemoryRegionId::parse(id);
    if (!parsed.ok()) {
      return fail(ErrorCode::CorruptState, "invalid region identity", id);
    }
    record.id = parsed.value();
    if (generation == 0) {
      return fail(ErrorCode::CorruptState, "region generation must be non-zero", id);
    }
    record.generation = MemoryRegionGeneration{generation};
    record.memory_domain = MemoryDomainId{domain};
    record.memory_domain_generation = MemoryDomainGeneration{domain_generation};
    record.coherence_domain = CoherenceDomainId{coherence};
    record.workload = WorkloadId{workload};
    record.opaque_handle = handle;
    record.size_bytes = size;
    record.page_size_bytes = page_size;
    record.allocation_generation = allocation_generation;
    record.mapping_generation = mapping_generation;
    record.sharing_scope = static_cast<SharingScope>(scope);
    record.size_known = size_known != 0;
    record.page_size_known = page_known != 0;
    record.retired = retired != 0;
    record.registered_at_ns = static_cast<Nanos>(registered);
    record.retired_at_ns = static_cast<Nanos>(retired_at);
    record.loaded_from_state = true;
    if (staged.regions.find(id) != staged.regions.end()) {
      return fail(ErrorCode::CorruptState, "duplicate region identity", id);
    }
    staged.regions[id] = std::move(record);
  }
  return reader.require_exhausted();
}

Status stage_topology(PayloadReader& reader, StagedState& staged) {
  std::uint64_t generation = 0;
  Status status = read_u64(reader, &generation);
  if (!status.ok()) return status;
  std::uint64_t count = 0;
  status = read_u64(reader, &count);
  if (!status.ok()) return status;
  if (count > Limits::kMaxTopologyLinkCount) {
    return fail(ErrorCode::CorruptState, "topology link count exceeds the bound");
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    TopologyLink link;
    std::uint8_t from_kind = 0;
    std::uint8_t to_kind = 0;
    std::uint8_t locality = 0;
    std::uint8_t provenance = 0;
    std::string from_id;
    std::string to_id;
    status = reader.u8(&from_kind);
    if (!status.ok()) return status;
    status = read_text(reader, &from_id, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    std::uint64_t from_generation = 0;
    status = read_u64(reader, &from_generation);
    if (!status.ok()) return status;
    status = reader.u8(&to_kind);
    if (!status.ok()) return status;
    status = read_text(reader, &to_id, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    std::uint64_t to_generation = 0;
    status = read_u64(reader, &to_generation);
    if (!status.ok()) return status;
    status = reader.u8(&locality);
    if (!status.ok()) return status;
    status = reader.u8(&provenance);
    if (!status.ok()) return status;
    std::int64_t latency = 0;
    status = reader.i64(&latency);
    if (!status.ok()) return status;
    std::uint64_t bandwidth = 0;
    status = read_u64(reader, &bandwidth);
    if (!status.ok()) return status;

    if (from_kind > 5 || to_kind > 5) {
      return fail(ErrorCode::CorruptState, "topology link resource kind out of range");
    }
    if (locality >= kLocalityCount) {
      return fail(ErrorCode::CorruptState, "topology link locality out of range");
    }
    if (provenance > 9) {
      return fail(ErrorCode::CorruptState, "topology link provenance out of range");
    }
    link.from.kind = static_cast<ResourceKind>(from_kind);
    link.from.id = ResourceId{from_id};
    link.from.generation = from_generation;
    link.to.kind = static_cast<ResourceKind>(to_kind);
    link.to.id = ResourceId{to_id};
    link.to.generation = to_generation;
    link.locality = static_cast<Locality>(locality);
    link.provenance = static_cast<Provenance>(provenance);
    link.latency_ns = latency;
    link.bandwidth_bytes_per_second = bandwidth;
    staged.links.push_back(std::move(link));
  }
  staged.topology_generation = TopologyGeneration{generation};
  return reader.require_exhausted();
}

Status stage_watermarks(PayloadReader& reader, StagedState& staged) {
  std::uint64_t count = 0;
  Status status = read_u64(reader, &count);
  if (!status.ok()) return status;
  if (count > Limits::kMaxPublisherWatermarks) {
    return fail(ErrorCode::CorruptState, "publisher watermark count exceeds the bound");
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    PublisherWatermark watermark;
    std::string publisher;
    status = read_text(reader, &publisher, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    std::uint64_t boot = 0;
    status = read_u64(reader, &boot);
    if (!status.ok()) return status;
    std::uint64_t high = 0;
    status = read_u64(reader, &high);
    if (!status.ok()) return status;
    std::uint8_t has = 0;
    status = reader.u8(&has);
    if (!status.ok()) return status;
    std::uint8_t fenced = 0;
    status = reader.u8(&fenced);
    if (!status.ok()) return status;
    std::uint8_t fence_reason = 0;
    status = reader.u8(&fence_reason);
    if (!status.ok()) return status;
    std::uint64_t incarnations = 0;
    status = read_u64(reader, &incarnations);
    if (!status.ok()) return status;
    std::uint64_t last_seen = 0;
    status = read_u64(reader, &last_seen);
    if (!status.ok()) return status;

    if (fence_reason > 5) {
      return fail(ErrorCode::CorruptState, "publisher watermark fence reason out of range");
    }
    const Result<PublisherId> parsed = PublisherId::parse(publisher);
    if (!parsed.ok()) {
      return fail(ErrorCode::CorruptState, "invalid publisher identity", publisher);
    }
    if (boot == 0) {
      return fail(ErrorCode::CorruptState, "publisher watermark boot must be non-zero",
                  publisher);
    }
    watermark.publisher = parsed.value();
    watermark.boot = PublisherBootId{boot};
    watermark.high_watermark = high;
    watermark.has_watermark = has != 0;
    watermark.fenced = fenced != 0;
    watermark.fence_reason = static_cast<FenceReason>(fence_reason);
    watermark.boot_incarnations = incarnations;
    watermark.last_seen_ns = static_cast<Nanos>(last_seen);
    const std::string key = boot_key(watermark.publisher, watermark.boot);
    if (staged.watermarks.find(key) != staged.watermarks.end()) {
      return fail(ErrorCode::CorruptState, "duplicate publisher watermark", key);
    }
    staged.watermarks[key] = std::move(watermark);
  }
  return reader.require_exhausted();
}

Status stage_capabilities(PayloadReader& reader, StagedState& staged) {
  std::uint64_t count = 0;
  Status status = read_u64(reader, &count);
  if (!status.ok()) return status;
  if (count > 256) {
    return fail(ErrorCode::CorruptState, "capability count exceeds the bound");
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    Capability capability;
    std::uint8_t capability_status = 0;
    status = read_text(reader, &capability.key, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    status = reader.u8(&capability_status);
    if (!status.ok()) return status;
    status = read_text(reader, &capability.detail, 512);
    if (!status.ok()) return status;
    status = read_text(reader, &capability.mechanism, 128);
    if (!status.ok()) return status;
    if (capability_status > 2) {
      return fail(ErrorCode::CorruptState, "capability status out of range", capability.key);
    }
    capability.status = static_cast<CapabilityStatus>(capability_status);
    if (capability.key.empty()) {
      return fail(ErrorCode::CorruptState, "capability key must not be empty");
    }
    if (staged.capabilities.find(capability.key) != staged.capabilities.end()) {
      return fail(ErrorCode::CorruptState, "duplicate capability key", capability.key);
    }
    staged.capabilities[capability.key] = std::move(capability);
  }
  return reader.require_exhausted();
}

Status stage_cost_model(PayloadReader& reader, StagedState& staged) {
  CostModel model;
  Status status = read_text(reader, &model.id, 96);
  if (!status.ok()) return status;
  std::uint32_t version = 0;
  status = read_u32(reader, &version);
  if (!status.ok()) return status;
  model.version = version;
  double* fields[] = {&model.remote_read_latency_ns,   &model.remote_write_latency_ns,
                      &model.invalidation_latency_ns,  &model.ownership_transfer_latency_ns,
                      &model.retry_latency_ns,         &model.conflict_stall_ns,
                      &model.bandwidth_bytes_per_ns,   &model.cxl_multiplier,
                      &model.pooled_multiplier,        &model.remote_node_multiplier};
  for (double* field : fields) {
    status = reader.f64(field);
    if (!status.ok()) return status;
  }
  model.defined = true;
  const Status valid = model.validate();
  if (!valid.ok()) {
    return fail(ErrorCode::CorruptState, "cost model in state file is invalid", valid.describe());
  }
  staged.cost_model = std::move(model);
  staged.has_cost_model = true;
  return reader.require_exhausted();
}

Status stage_historical_aggregates(PayloadReader& reader, StagedState& staged) {
  std::uint64_t count = 0;
  Status status = read_u64(reader, &count);
  if (!status.ok()) return status;
  if (count > Limits::kMaxHistoryAggregates) {
    return fail(ErrorCode::CorruptState, "historical aggregate count exceeds the bound");
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    AggregateKey key;
    AggregateValue value;
    std::uint8_t dimension = 0;
    status = reader.u8(&dimension);
    if (!status.ok()) return status;
    if (dimension >= kAggregateDimensionCount) {
      return fail(ErrorCode::CorruptState, "historical aggregate dimension out of range");
    }
    key.dimension = static_cast<AggregateDimension>(dimension);
    status = read_text(reader, &key.value, Limits::kMaxRegionAnnotationLength);
    if (!status.ok()) return status;
    status = read_u64(reader, &value.observations);
    if (!status.ok()) return status;
    status = read_u64(reader, &value.bytes);
    if (!status.ok()) return status;
    status = read_u64(reader, &value.invalidations);
    if (!status.ok()) return status;
    status = read_u64(reader, &value.ownership_transfers);
    if (!status.ok()) return status;
    std::uint8_t precision = 0;
    status = reader.u8(&precision);
    if (!status.ok()) return status;
    if (precision > 6) {
      return fail(ErrorCode::CorruptState, "historical aggregate precision out of range");
    }
    value.weakest_precision = static_cast<Precision>(precision);
    value.has_observations = value.observations != 0;
    std::uint8_t synthetic = 0;
    status = reader.u8(&synthetic);
    if (!status.ok()) return status;
    value.synthetic_contribution = synthetic != 0;
    if (staged.historical_aggregates.find(key) != staged.historical_aggregates.end()) {
      return fail(ErrorCode::CorruptState, "duplicate historical aggregate key", key.value);
    }
    staged.historical_aggregates[key] = value;
  }
  staged.record_count += count;
  return reader.require_exhausted();
}

Status stage_history(PayloadReader& reader, StagedState& staged,
                     const PersistenceOptions& options) {
  std::uint64_t count = 0;
  Status status = read_u64(reader, &count);
  if (!status.ok()) return status;
  if (count > options.max_history_records) {
    return fail(ErrorCode::CorruptState, "history record count exceeds the configured bound");
  }
  for (std::uint64_t i = 0; i < count; ++i) {
    HistoricalEvidence record;
    std::uint8_t type = 0;
    std::uint8_t precision = 0;
    std::uint8_t provenance = 0;
    std::string region;
    status = read_text(reader, &region, Limits::kMaxNameLength);
    if (!status.ok()) return status;
    status = reader.u8(&type);
    if (!status.ok()) return status;
    status = reader.u8(&precision);
    if (!status.ok()) return status;
    status = reader.u8(&provenance);
    if (!status.ok()) return status;
    status = read_u64(reader, &record.observations);
    if (!status.ok()) return status;
    status = read_u64(reader, &record.bytes);
    if (!status.ok()) return status;
    std::uint64_t last = 0;
    status = read_u64(reader, &last);
    if (!status.ok()) return status;
    status = read_u64(reader, &record.source_coordinator_epoch);
    if (!status.ok()) return status;

    if (type >= kEventTypeCount) {
      return fail(ErrorCode::CorruptState, "history event type out of range");
    }
    if (precision > 6 || provenance > 9) {
      return fail(ErrorCode::CorruptState, "history classification out of range");
    }
    record.region = MemoryRegionId{region};
    record.event_type = static_cast<EventType>(type);
    record.precision = static_cast<Precision>(precision);
    record.provenance = static_cast<Provenance>(provenance);
    record.last_timestamp_ns = static_cast<Nanos>(last);
    staged.history.push_back(std::move(record));
  }
  staged.record_count += count;
  return reader.require_exhausted();
}

Status parse_state(const std::vector<std::uint8_t>& data, const PersistenceOptions& options,
                   StagedState* staged) {
  if (data.size() < Limits::kStateHeaderSize + Limits::kStateTrailerSize) {
    return fail(ErrorCode::EmptyState, "state file is shorter than its fixed envelope");
  }
  if (std::memcmp(data.data(), kStateMagic, 8) != 0) {
    return fail(ErrorCode::InvalidMagic, "state file magic mismatch");
  }
  const std::uint32_t version = get_u32(data.data() + 8);
  if (version != Limits::kStateVersion) {
    return fail(ErrorCode::UnsupportedStateVersion, "unsupported state file version",
                format_u64(version));
  }
  const std::uint32_t flags = get_u32(data.data() + 12);
  if (flags != 0) {
    return fail(ErrorCode::CorruptState, "unknown state file flags", format_u64(flags));
  }
  const std::uint64_t payload_length = get_u64(data.data() + 16);
  const std::uint64_t declared_coordinator = get_u64(data.data() + 24);
  const std::uint64_t declared_observation = get_u64(data.data() + 32);
  const std::uint64_t declared_records = get_u64(data.data() + 40);
  const std::uint32_t header_crc = get_u32(data.data() + 48);
  {
    std::array<std::uint8_t, Limits::kStateHeaderSize> copy{};
    std::memcpy(copy.data(), data.data(), Limits::kStateHeaderSize);
    put_u32(copy.data() + 48, 0);
    if (crc32(copy.data(), copy.size()) != header_crc) {
      return fail(ErrorCode::IntegrityFailure, "state header checksum mismatch");
    }
  }
  if (payload_length > options.max_bytes) {
    return fail(ErrorCode::TooLarge, "state payload exceeds the accepted bound");
  }
  const std::uint64_t expected =
      static_cast<std::uint64_t>(Limits::kStateHeaderSize) + payload_length +
      static_cast<std::uint64_t>(Limits::kStateTrailerSize);
  if (data.size() < expected) {
    return fail(ErrorCode::Truncated, "state file is truncated",
                std::string("expected=") + format_u64(expected) +
                    " actual=" + format_u64(data.size()));
  }
  if (data.size() > expected) {
    return fail(ErrorCode::CorruptState, "trailing bytes follow the state payload");
  }
  const std::uint8_t* payload = data.data() + Limits::kStateHeaderSize;
  const std::uint8_t* trailer = payload + payload_length;
  if (std::memcmp(trailer, kStateTrailerMagic, 8) != 0) {
    return fail(ErrorCode::InvalidMagic, "state trailer magic mismatch");
  }
  const std::uint32_t payload_crc = get_u32(trailer + 8);
  if (crc32(payload, static_cast<std::size_t>(payload_length)) != payload_crc) {
    return fail(ErrorCode::IntegrityFailure, "state payload checksum mismatch");
  }

  staged->source_coordinator_epoch = CoordinatorEpoch{declared_coordinator};
  staged->source_observation_epoch = ObservationEpoch{declared_observation};

  std::size_t offset = 0;
  std::uint64_t stated_records = 0;
  while (offset < payload_length) {
    if (payload_length - offset < Limits::kStateSectionHeaderSize) {
      return fail(ErrorCode::Truncated, "state section header is truncated");
    }
    const std::uint32_t raw_type = get_u32(payload + offset);
    const std::uint32_t section_flags = get_u32(payload + offset + 4);
    const std::uint64_t section_length = get_u64(payload + offset + 8);
    if (section_flags != 0) {
      return fail(ErrorCode::CorruptState, "unknown state section flags");
    }
    if (section_length < 4) {
      return fail(ErrorCode::CorruptState, "state section length is too small");
    }
    if (payload_length - offset - Limits::kStateSectionHeaderSize < section_length) {
      return fail(ErrorCode::Truncated, "state section body is truncated");
    }
    const std::uint8_t* body = payload + offset + Limits::kStateSectionHeaderSize;
    const std::size_t body_size = static_cast<std::size_t>(section_length) - 4;
    const std::uint32_t body_crc = get_u32(body + body_size);
    if (crc32(body, body_size) != body_crc) {
      return fail(ErrorCode::IntegrityFailure, "state section checksum mismatch",
                  format_u64(raw_type));
    }
    if (!staged->seen_sections.insert(raw_type).second) {
      return fail(ErrorCode::CorruptState, "duplicate state section", format_u64(raw_type));
    }

    PayloadReader reader(body, body_size);
    Status status;
    switch (static_cast<SectionType>(raw_type)) {
      case SectionType::Meta:
        status = stage_meta(reader, *staged);
        break;
      case SectionType::Nodes:
        status = stage_nodes(reader, *staged);
        break;
      case SectionType::Processors:
        status = stage_processors(reader, *staged);
        break;
      case SectionType::Accelerators:
        status = stage_accelerators(reader, *staged);
        break;
      case SectionType::MemoryDomains:
        status = stage_memory_domains(reader, *staged);
        break;
      case SectionType::CoherenceDomains:
        status = stage_coherence_domains(reader, *staged);
        break;
      case SectionType::Regions:
        status = stage_regions(reader, *staged);
        break;
      case SectionType::Topology:
        status = stage_topology(reader, *staged);
        break;
      case SectionType::PublisherWatermarks:
        status = stage_watermarks(reader, *staged);
        break;
      case SectionType::Capabilities:
        status = stage_capabilities(reader, *staged);
        break;
      case SectionType::CostModel:
        status = stage_cost_model(reader, *staged);
        break;
      case SectionType::HistoricalAggregates:
        status = stage_historical_aggregates(reader, *staged);
        break;
      case SectionType::History:
        status = stage_history(reader, *staged, options);
        break;
      default:
        return fail(ErrorCode::CorruptState, "unknown state section type",
                    format_u64(raw_type));
    }
    if (!status.ok()) {
      return status;
    }
    offset += Limits::kStateSectionHeaderSize + static_cast<std::size_t>(section_length);
    stated_records += 1;
  }
  if (offset != payload_length) {
    return fail(ErrorCode::CorruptState, "state payload is not exactly consumed");
  }
  if (!staged->has_meta) {
    return fail(ErrorCode::CorruptState, "state file has no meta section");
  }
  if (declared_records != staged->record_count) {
    return fail(ErrorCode::CorruptState, "state record count does not match its payload",
                std::string("declared=") + format_u64(declared_records) +
                    " actual=" + format_u64(staged->record_count));
  }

  // Referential integrity: a region must name a memory domain that exists.
  for (const auto& entry : staged->regions) {
    if (entry.second.memory_domain.empty()) {
      continue;
    }
    if (staged->memory_domains.find(entry.second.memory_domain.str()) ==
        staged->memory_domains.end()) {
      return fail(ErrorCode::CorruptState, "region references an unknown memory domain",
                  entry.first);
    }
  }
  for (const auto& entry : staged->memory_domains) {
    if (entry.second.coherence_domain.empty()) {
      continue;
    }
    if (staged->coherence_domains.find(entry.second.coherence_domain.str()) ==
        staged->coherence_domains.end()) {
      return fail(ErrorCode::CorruptState, "memory domain references an unknown coherence domain",
                  entry.first);
    }
  }
  for (const auto& entry : staged->processors) {
    if (entry.second.node.empty()) {
      continue;
    }
    if (staged->nodes.find(entry.second.node.str()) == staged->nodes.end()) {
      return fail(ErrorCode::CorruptState, "processor references an unknown node", entry.first);
    }
  }
  for (const TopologyLink& link : staged->links) {
    if (link.from.empty() || link.to.empty()) {
      return fail(ErrorCode::CorruptState, "topology link endpoint is empty");
    }
  }
  return Status();
}

}  // namespace

std::string_view state_magic() noexcept { return std::string_view(kStateMagic, 8); }

Result<PersistenceReport> save_state(const State& state, const std::filesystem::path& path,
                                     const PersistenceOptions& options) {
  std::vector<std::uint8_t> payload;
  PersistenceReport report;
  report.source_coordinator_epoch = state.coordinator_epoch;

  auto append_section = [&payload](SectionType type, const std::vector<std::uint8_t>& body) {
    const std::vector<std::uint8_t> section = build_section(type, body);
    payload.insert(payload.end(), section.begin(), section.end());
  };

  {
    PayloadWriter writer;
    writer.u32(1);
    writer.text(state.options.observer_id.view(), Limits::kMaxNameLength);
    writer.text(state.options.node_id.view(), Limits::kMaxNameLength);
    writer.text(state.options.display_name, Limits::kMaxRegionAnnotationLength);
    writer.u64(state.topology_generation.value());
    writer.u64(state.evidence_generation.value());
    writer.u64(static_cast<std::uint64_t>(state.options.time_bucket_ns));
    writer.u64(state.coordinator_epoch.value());
    writer.u64(state.observation_epoch.value());
    append_section(SectionType::Meta, writer.data());
    report.sections += 1;
  }
  {
    PayloadWriter writer;
    writer.u64(state.nodes.size());
    for (const auto& entry : state.nodes) {
      writer.text(entry.second.id.view(), Limits::kMaxNameLength);
      writer.text(entry.second.display_name, Limits::kMaxRegionAnnotationLength);
      writer.u64(static_cast<std::uint64_t>(entry.second.registered_at_ns));
    }
    append_section(SectionType::Nodes, writer.data());
    report.sections += 1;
  }
  {
    PayloadWriter writer;
    writer.u64(state.processors.size());
    for (const auto& entry : state.processors) {
      const ProcessorRecord& record = entry.second;
      writer.text(record.id.view(), Limits::kMaxNameLength);
      writer.text(record.node.view(), Limits::kMaxNameLength);
      writer.text(record.local_memory_domain.view(), Limits::kMaxNameLength);
      writer.u64(record.generation.value());
      writer.u32(record.logical_processor_count);
      writer.u32(record.physical_core_count);
      writer.u32(record.numa_node_index);
      writer.u8(record.numa_node_index_known);
      writer.u8(record.retired);
      writer.u64(static_cast<std::uint64_t>(record.registered_at_ns));
    }
    append_section(SectionType::Processors, writer.data());
    report.processors = state.processors.size();
    report.sections += 1;
  }
  {
    PayloadWriter writer;
    writer.u64(state.accelerators.size());
    for (const auto& entry : state.accelerators) {
      const AcceleratorRecord& record = entry.second;
      writer.text(record.id.view(), Limits::kMaxNameLength);
      writer.text(record.node.view(), Limits::kMaxNameLength);
      writer.text(record.vendor, Limits::kMaxNameLength);
      writer.text(record.vendor_uuid, Limits::kMaxRegionAnnotationLength);
      writer.text(record.local_memory_domain.view(), Limits::kMaxNameLength);
      writer.u64(record.generation.value());
      writer.u8(record.peer_coherent_capable);
      writer.u8(record.peer_coherent_capability_known);
      writer.u8(record.retired);
      writer.u64(static_cast<std::uint64_t>(record.registered_at_ns));
    }
    append_section(SectionType::Accelerators, writer.data());
    report.accelerators = state.accelerators.size();
    report.sections += 1;
  }
  {
    PayloadWriter writer;
    writer.u64(state.memory_domains.size());
    for (const auto& entry : state.memory_domains) {
      const MemoryDomainRecord& record = entry.second;
      writer.text(record.id.view(), Limits::kMaxNameLength);
      writer.text(record.node.view(), Limits::kMaxNameLength);
      writer.text(record.coherence_domain.view(), Limits::kMaxNameLength);
      writer.u64(record.generation.value());
      writer.u8(static_cast<std::uint8_t>(record.kind));
      writer.u8(record.coherent_with_host);
      writer.u8(record.coherent_with_host_known);
      writer.u8(record.capacity_known);
      writer.u8(record.retired);
      writer.u64(record.capacity_bytes);
      writer.u64(static_cast<std::uint64_t>(record.registered_at_ns));
    }
    append_section(SectionType::MemoryDomains, writer.data());
    report.memory_domains = state.memory_domains.size();
    report.sections += 1;
  }
  {
    PayloadWriter writer;
    writer.u64(state.coherence_domains.size());
    for (const auto& entry : state.coherence_domains) {
      const CoherenceDomainRecord& record = entry.second;
      writer.text(record.id.view(), Limits::kMaxNameLength);
      writer.text(record.protocol_family, Limits::kMaxRegionAnnotationLength);
      writer.u64(record.generation.value());
      writer.u8(record.retired);
      writer.u64(record.members.size());
      for (const ResourceRef& member : record.members) {
        writer.u8(static_cast<std::uint8_t>(member.kind));
        writer.text(member.id.view(), Limits::kMaxNameLength);
        writer.u64(member.generation);
      }
      writer.u64(static_cast<std::uint64_t>(record.registered_at_ns));
    }
    append_section(SectionType::CoherenceDomains, writer.data());
    report.coherence_domains = state.coherence_domains.size();
    report.sections += 1;
  }
  {
    PayloadWriter writer;
    writer.u64(state.regions.size());
    for (const auto& entry : state.regions) {
      const RegionRecord& record = entry.second;
      writer.text(record.id.view(), Limits::kMaxNameLength);
      writer.text(record.owner, Limits::kMaxRegionAnnotationLength);
      writer.text(record.annotation, Limits::kMaxRegionAnnotationLength);
      writer.text(record.memory_domain.view(), Limits::kMaxNameLength);
      writer.text(record.coherence_domain.view(), Limits::kMaxNameLength);
      writer.text(record.workload.view(), Limits::kMaxNameLength);
      writer.u64(record.generation.value());
      writer.u64(record.memory_domain_generation.value());
      writer.u64(record.opaque_handle);
      writer.u64(record.size_bytes);
      writer.u64(record.page_size_bytes);
      writer.u64(record.allocation_generation);
      writer.u64(record.mapping_generation);
      writer.u8(static_cast<std::uint8_t>(record.sharing_scope));
      writer.u8(record.size_known);
      writer.u8(record.page_size_known);
      writer.u8(record.retired);
      writer.u64(static_cast<std::uint64_t>(record.registered_at_ns));
      writer.u64(static_cast<std::uint64_t>(record.retired_at_ns));
    }
    append_section(SectionType::Regions, writer.data());
    report.regions = state.regions.size();
    report.sections += 1;
  }
  {
    PayloadWriter writer;
    writer.u64(state.topology_generation.value());
    writer.u64(state.topology.links.size());
    for (const TopologyLink& link : state.topology.links) {
      writer.u8(static_cast<std::uint8_t>(link.from.kind));
      writer.text(link.from.id.view(), Limits::kMaxNameLength);
      writer.u64(link.from.generation);
      writer.u8(static_cast<std::uint8_t>(link.to.kind));
      writer.text(link.to.id.view(), Limits::kMaxNameLength);
      writer.u64(link.to.generation);
      writer.u8(static_cast<std::uint8_t>(link.locality));
      writer.u8(static_cast<std::uint8_t>(link.provenance));
      writer.i64(link.latency_ns);
      writer.u64(link.bandwidth_bytes_per_second);
    }
    append_section(SectionType::Topology, writer.data());
    report.topology_links = state.topology.links.size();
    report.sections += 1;
  }
  {
    // Live publisher watermarks are persisted alongside fenced ones so that a
    // restart can refuse a replay of anything already accepted.
    std::map<std::string, PublisherWatermark> watermarks = state.watermarks;
    for (const auto& entry : state.publishers) {
      const std::string key = boot_key(entry.second.view.id, entry.second.view.boot);
      if (watermarks.find(key) != watermarks.end()) {
        continue;
      }
      PublisherWatermark watermark;
      watermark.publisher = entry.second.view.id;
      watermark.boot = entry.second.view.boot;
      watermark.high_watermark = entry.second.high_watermark;
      watermark.has_watermark = entry.second.has_watermark;
      watermark.fenced = entry.second.view.fenced;
      watermark.fence_reason = entry.second.view.fence_reason;
      watermark.boot_incarnations = entry.second.boot_incarnations;
      watermark.last_seen_ns = entry.second.view.last_seen_ns;
      watermarks[key] = watermark;
    }
    PayloadWriter writer;
    writer.u64(watermarks.size());
    for (const auto& entry : watermarks) {
      const PublisherWatermark& watermark = entry.second;
      writer.text(watermark.publisher.view(), Limits::kMaxNameLength);
      writer.u64(watermark.boot.value());
      writer.u64(watermark.high_watermark);
      writer.u8(watermark.has_watermark);
      writer.u8(watermark.fenced);
      writer.u8(static_cast<std::uint8_t>(watermark.fence_reason));
      writer.u64(watermark.boot_incarnations);
      writer.u64(static_cast<std::uint64_t>(watermark.last_seen_ns));
    }
    append_section(SectionType::PublisherWatermarks, writer.data());
    report.publisher_watermarks = watermarks.size();
    report.sections += 1;
  }
  {
    PayloadWriter writer;
    writer.u64(state.capabilities.size());
    for (const auto& entry : state.capabilities) {
      writer.text(entry.second.key, Limits::kMaxNameLength);
      writer.u8(static_cast<std::uint8_t>(entry.second.status));
      writer.text(entry.second.detail, 512);
      writer.text(entry.second.mechanism, 128);
    }
    append_section(SectionType::Capabilities, writer.data());
    report.sections += 1;
  }
  {
    PayloadWriter writer;
    const CostModel& model = state.options.cost_model;
    writer.text(model.id, 96);
    writer.u32(model.version);
    writer.f64(model.remote_read_latency_ns);
    writer.f64(model.remote_write_latency_ns);
    writer.f64(model.invalidation_latency_ns);
    writer.f64(model.ownership_transfer_latency_ns);
    writer.f64(model.retry_latency_ns);
    writer.f64(model.conflict_stall_ns);
    writer.f64(model.bandwidth_bytes_per_ns);
    writer.f64(model.cxl_multiplier);
    writer.f64(model.pooled_multiplier);
    writer.f64(model.remote_node_multiplier);
    append_section(SectionType::CostModel, writer.data());
    report.sections += 1;
  }

  std::uint64_t history_records = 0;
  std::uint64_t history_aggregates = 0;
  if (options.include_history) {
    {
      PayloadWriter writer;
      std::size_t written = 0;
      for (const auto& entry : state.historical_aggregates.buckets()) {
        if (written >= options.max_history_records) {
          break;
        }
        writer.u8(static_cast<std::uint8_t>(entry.first.dimension));
        writer.text(entry.first.value, Limits::kMaxRegionAnnotationLength);
        writer.u64(entry.second.observations);
        writer.u64(entry.second.bytes);
        writer.u64(entry.second.invalidations);
        writer.u64(entry.second.ownership_transfers);
        writer.u8(static_cast<std::uint8_t>(entry.second.weakest_precision));
        writer.u8(entry.second.synthetic_contribution);
        ++written;
      }
      // The header count is written first, so build the body in two passes.
      PayloadWriter final_writer;
      final_writer.u64(written);
      final_writer.bytes(writer.data().data(), writer.data().size());
      append_section(SectionType::HistoricalAggregates, final_writer.data());
      history_aggregates = written;
      report.sections += 1;
    }
    {
      PayloadWriter writer;
      std::size_t written = 0;
      for (const HistoricalEvidence& record : state.history) {
        if (written >= options.max_history_records) {
          break;
        }
        writer.text(record.region.view(), Limits::kMaxNameLength);
        writer.u8(static_cast<std::uint8_t>(record.event_type));
        writer.u8(static_cast<std::uint8_t>(record.precision));
        writer.u8(static_cast<std::uint8_t>(record.provenance));
        writer.u64(record.observations);
        writer.u64(record.bytes);
        writer.u64(static_cast<std::uint64_t>(record.last_timestamp_ns));
        writer.u64(record.source_coordinator_epoch);
        ++written;
      }
      PayloadWriter final_writer;
      final_writer.u64(written);
      final_writer.bytes(writer.data().data(), writer.data().size());
      append_section(SectionType::History, final_writer.data());
      history_records = written;
      report.sections += 1;
    }
  }

  const std::uint64_t record_count = history_records + history_aggregates;
  std::vector<std::uint8_t> output;
  output.resize(Limits::kStateHeaderSize + payload.size() + Limits::kStateTrailerSize);
  std::memcpy(output.data(), kStateMagic, 8);
  put_u32(output.data() + 8, Limits::kStateVersion);
  put_u32(output.data() + 12, 0);
  put_u64(output.data() + 16, payload.size());
  put_u64(output.data() + 24, state.coordinator_epoch.value());
  put_u64(output.data() + 32, state.observation_epoch.value());
  put_u64(output.data() + 40, record_count);
  put_u32(output.data() + 48, 0);
  put_u32(output.data() + 52, 0);
  put_u64(output.data() + 56, 0);
  put_u32(output.data() + 48, crc32(output.data(), Limits::kStateHeaderSize));
  std::memcpy(output.data() + Limits::kStateHeaderSize, payload.data(), payload.size());
  std::uint8_t* trailer = output.data() + Limits::kStateHeaderSize + payload.size();
  std::memcpy(trailer, kStateTrailerMagic, 8);
  put_u32(trailer + 8, crc32(payload.data(), payload.size()));
  put_u32(trailer + 12, 0);

  const Status written = options.atomic_replace
                             ? write_file_atomic(path, output.data(), output.size())
                             : Status();
  if (!options.atomic_replace) {
    std::FILE* file = nullptr;
#if defined(_WIN32)
    if (fopen_s(&file, path.string().c_str(), "wb") != 0) {
      file = nullptr;
    }
#else
    file = std::fopen(path.string().c_str(), "wb");
#endif
    if (file == nullptr) {
      return fail_as<PersistenceReport>(ErrorCode::IoError, "cannot open state file",
                                        path.string());
    }
    const std::size_t put = std::fwrite(output.data(), 1, output.size(), file);
    std::fclose(file);
    if (put != output.size()) {
      return fail_as<PersistenceReport>(ErrorCode::IoError, "short write to state file",
                                        path.string());
    }
  } else if (!written.ok()) {
    return Result<PersistenceReport>(written.error());
  }

  report.bytes_written = output.size();
  report.history_records = history_records;
  report.history_aggregates = history_aggregates;
  report.atomic_replacement = options.atomic_replace;
  report.next_coordinator_epoch = state.coordinator_epoch;
  return Result<PersistenceReport>(std::move(report));
}

Result<PersistenceReport> load_state(State& state, const std::filesystem::path& path,
                                     const PersistenceOptions& options) {
  const Result<std::vector<std::uint8_t>> data = read_file_bounded(path, options.max_bytes);
  if (!data.ok()) {
    return Result<PersistenceReport>(data.error());
  }
  StagedState staged;
  const Status parsed = parse_state(data.value(), options, &staged);
  if (!parsed.ok()) {
    return Result<PersistenceReport>(parsed.error());
  }

  // The whole file has been accepted: apply recovery.
  PersistenceReport report;
  report.bytes_read = data.value().size();
  report.regions = staged.regions.size();
  report.processors = staged.processors.size();
  report.accelerators = staged.accelerators.size();
  report.memory_domains = staged.memory_domains.size();
  report.coherence_domains = staged.coherence_domains.size();
  report.topology_links = staged.links.size();
  report.publisher_watermarks = staged.watermarks.size();
  report.history_records = staged.history.size();
  report.history_aggregates = staged.historical_aggregates.size();
  report.source_coordinator_epoch = staged.source_coordinator_epoch;

  state.coordinator_epoch = staged.source_coordinator_epoch.next();
  state.observation_epoch =
      ObservationEpoch{staged.source_observation_epoch.value() + 1};
  report.next_coordinator_epoch = state.coordinator_epoch;

  if (!staged.observer_id.empty()) {
    state.options.observer_id = staged.observer_id;
  }
  if (!staged.node_id.empty()) {
    state.options.node_id = staged.node_id;
  }
  if (!staged.display_name.empty()) {
    state.options.display_name = staged.display_name;
  }
  if (staged.time_bucket_ns > 0) {
    state.options.time_bucket_ns = staged.time_bucket_ns;
  }
  if (staged.has_cost_model) {
    state.options.cost_model = staged.cost_model;
  }

  state.nodes = std::move(staged.nodes);
  state.processors = std::move(staged.processors);
  state.accelerators = std::move(staged.accelerators);
  state.memory_domains = std::move(staged.memory_domains);
  state.coherence_domains = std::move(staged.coherence_domains);
  state.regions = std::move(staged.regions);
  state.topology.links = std::move(staged.links);
  state.topology.generation = staged.topology_generation;
  state.topology.loaded_from_state = true;
  state.topology.created_at_ns = monotonic_now_ns();
  state.topology_generation = staged.topology_generation;
  state.evidence_generation = staged.evidence_generation.is_zero()
                                  ? EvidenceGeneration{1}
                                  : staged.evidence_generation;

  for (auto& entry : state.regions) {
    entry.second.loaded_from_state = true;
  }

  // Publishers are never restored as current.  Their replay watermarks are
  // preserved so that prior-run traffic cannot be replayed: a boot that
  // re-registers after a restart resumes from its watermark, and any frame at
  // or below it is rejected as a stale replay.
  for (auto& entry : staged.watermarks) {
    entry.second.fenced = false;
    entry.second.fence_reason = FenceReason::CoordinatorRestart;
  }
  state.watermarks = std::move(staged.watermarks);
  while (state.watermarks.size() > Limits::kMaxPublisherWatermarks) {
    state.watermarks.erase(state.watermarks.begin());
  }

  for (auto& entry : staged.capabilities) {
    state.capabilities[entry.first] = std::move(entry.second);
  }

  // History stays historical: it never enters current aggregates.
  state.historical_aggregates.restore(std::move(staged.historical_aggregates));
  state.history = std::move(staged.history);
  state.historical_observation_count = staged.record_count;
  state.history_source_epoch = staged.source_coordinator_epoch;

  state.aggregates.clear();
  state.journal.clear();
  state.region_evidence.clear();
  state.region_evidence_order.clear();
  state.counters.clear();
  state.publishers.clear();
  state.stale.clear();

  StaleEvidenceRecord record;
  record.kind = StaleEvidenceRecord::Kind::StaleEpoch;
  record.observed_at_ns = monotonic_now_ns();
  record.detail = "coordinator epoch advanced to " +
                  format_u64(state.coordinator_epoch.value()) +
                  " by durable recovery; prior publishers are not current and must re-register";
  record_stale(state, std::move(record));
  return Result<PersistenceReport>(std::move(report));
}

}  // namespace detail
}  // namespace sol::coherence
