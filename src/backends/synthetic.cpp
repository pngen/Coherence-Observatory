// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/backends/synthetic.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <string>
#include <vector>

#include "coherence/limits.hpp"
#include "text_util.hpp"

namespace sol::coherence {
namespace {

constexpr std::string_view kNodeId = "node.synthetic.0";
constexpr std::string_view kObserverId = "observer.local";
constexpr std::string_view kCoherenceDomain = "cd.synthetic.0";
constexpr std::string_view kCoherenceDomainExact = "cd.synthetic.exact";
constexpr std::string_view kDomainHost = "md.host";
constexpr std::string_view kDomainAccA = "md.acc.a";
constexpr std::string_view kDomainAccB = "md.acc.b";
constexpr std::string_view kDomainCxl = "md.cxl";
constexpr std::string_view kDomainPooled = "md.pooled";
constexpr std::string_view kProc = "cpu.synthetic.0";
constexpr std::string_view kAccA = "acc.synthetic.a";
constexpr std::string_view kAccB = "acc.synthetic.b";
constexpr std::string_view kRegionShared = "region.shared";
constexpr std::string_view kRegionPing = "region.pingpong";
constexpr std::string_view kRegionLine = "region.line";
constexpr std::string_view kRegionCxl = "region.cxl";
constexpr std::string_view kRegionPage = "region.page";

constexpr std::size_t kMaxExpandedSteps = 200000;

struct PresetTable {
  SyntheticPreset preset;
  std::string_view name;
};

constexpr std::array<PresetTable, 10> kPresetNames = {{
    {SyntheticPreset::SteadySharedReads, "STEADY_SHARED_READS"},
    {SyntheticPreset::RemoteNumaAccess, "REMOTE_NUMA_ACCESS"},
    {SyntheticPreset::PingPong, "PING_PONG"},
    {SyntheticPreset::FalseSharingLike, "FALSE_SHARING_LIKE"},
    {SyntheticPreset::InvalidationBurst, "INVALIDATION_BURST"},
    {SyntheticPreset::CacheToCacheAndWriteback, "CACHE_TO_CACHE_AND_WRITEBACK"},
    {SyntheticPreset::CxlClassTraffic, "CXL_CLASS_TRAFFIC"},
    {SyntheticPreset::SnoopDirectoryRetry, "SNOOP_DIRECTORY_RETRY"},
    {SyntheticPreset::AggregateOnlyCounter, "AGGREGATE_ONLY_COUNTER"},
    {SyntheticPreset::PageGranularityContention, "PAGE_GRANULARITY_CONTENTION"},
}};

std::vector<SyntheticResourceSpec> base_resources() {
  std::vector<SyntheticResourceSpec> resources;
  auto add = [&resources](ResourceKind kind, std::string id, std::string node) {
    SyntheticResourceSpec spec;
    spec.kind = kind;
    spec.id = std::move(id);
    spec.node = std::move(node);
    resources.push_back(std::move(spec));
  };
  add(ResourceKind::Processor, std::string(kProc), std::string(kNodeId));
  add(ResourceKind::Accelerator, std::string(kAccA), std::string(kNodeId));
  add(ResourceKind::Accelerator, std::string(kAccB), std::string(kNodeId));
  add(ResourceKind::MemoryDomain, std::string(kDomainHost), std::string(kNodeId));
  add(ResourceKind::MemoryDomain, std::string(kDomainAccA), std::string(kNodeId));
  add(ResourceKind::MemoryDomain, std::string(kDomainAccB), std::string(kNodeId));
  add(ResourceKind::MemoryDomain, std::string(kDomainCxl), std::string(kNodeId));
  add(ResourceKind::MemoryDomain, std::string(kDomainPooled), std::string(kNodeId));
  for (SyntheticResourceSpec& spec : resources) {
    if (spec.id == kDomainCxl) {
      spec.domain_kind = MemoryDomainKind::CxlAttached;
      spec.coherent_with_host = false;
    } else if (spec.id == kDomainPooled) {
      spec.domain_kind = MemoryDomainKind::PooledMemory;
      spec.coherent_with_host = false;
    } else if (spec.id == kDomainAccA || spec.id == kDomainAccB) {
      spec.domain_kind = MemoryDomainKind::DeviceMemory;
      spec.coherent_with_host = true;
    } else if (spec.id == kDomainHost) {
      spec.domain_kind = MemoryDomainKind::HostDram;
      spec.coherent_with_host = true;
    }
    spec.coherence_domain = std::string(kCoherenceDomain);
    if (spec.kind == ResourceKind::Accelerator) {
      spec.vendor = "synthetic";
      spec.peer_coherent_capable = true;
    }
    if (spec.kind == ResourceKind::Processor) {
      spec.logical_processors = 8;
    }
  }
  return resources;
}

std::vector<SyntheticRegionSpec> base_regions() {
  std::vector<SyntheticRegionSpec> regions;
  auto add = [&regions](std::string id, std::string domain, std::uint64_t size,
                        SharingScope scope) {
    SyntheticRegionSpec spec;
    spec.id = std::move(id);
    spec.memory_domain = std::move(domain);
    spec.coherence_domain = std::string(kCoherenceDomain);
    spec.size_bytes = size;
    spec.sharing_scope = scope;
    spec.workload = "workload.synthetic";
    regions.push_back(std::move(spec));
  };
  add(std::string(kRegionShared), std::string(kDomainHost), 1u << 20, SharingScope::DeviceShared);
  add(std::string(kRegionPing), std::string(kDomainAccA), 4096, SharingScope::DeviceShared);
  add(std::string(kRegionLine), std::string(kDomainHost), 4096, SharingScope::ProcessShared);
  add(std::string(kRegionCxl), std::string(kDomainCxl), 1u << 20, SharingScope::Pooled);
  add(std::string(kRegionPage), std::string(kDomainHost), 1u << 21, SharingScope::ProcessShared);
  return regions;
}

SyntheticStep make_step(EventType type, std::string region, std::string source,
                        std::string target, std::uint32_t repeat) {
  SyntheticStep step;
  step.type = type;
  step.region = std::move(region);
  step.source = std::move(source);
  step.target = std::move(target);
  step.repeat = repeat;
  step.granularity = EvidenceGranularity::Region;
  step.precision = Precision::ExactEvent;
  step.provenance = Provenance::SyntheticBackend;
  step.spacing_ns = 1000;
  return step;
}

}  // namespace

std::string_view to_string(SyntheticPreset preset) noexcept {
  for (const PresetTable& entry : kPresetNames) {
    if (entry.preset == preset) {
      return entry.name;
    }
  }
  return "UNKNOWN";
}

Result<SyntheticPreset> parse_synthetic_preset(std::string_view text) {
  for (const PresetTable& entry : kPresetNames) {
    if (detail::iequals(entry.name, text)) {
      return Result<SyntheticPreset>(entry.preset);
    }
  }
  return fail_as<SyntheticPreset>(ErrorCode::InvalidArgument, "unknown synthetic preset",
                                  std::string(text));
}

std::vector<SyntheticStep> make_preset_script(SyntheticPreset preset) {
  std::vector<SyntheticStep> script;
  switch (preset) {
    case SyntheticPreset::SteadySharedReads: {
      SyntheticStep step =
          make_step(EventType::SharedRead, std::string(kRegionShared), std::string(kAccA),
                    std::string(kDomainHost), 48);
      step.direction = AccessDirection::Read;
      step.locality = Locality::LocalAccelerator;
      step.locality_declared = true;
      step.bytes = 64;
      step.lines = 1;
      script.push_back(step);
      SyntheticStep second =
          make_step(EventType::SharedRead, std::string(kRegionShared), std::string(kAccB),
                    std::string(kDomainHost), 48);
      second.direction = AccessDirection::Read;
      second.locality = Locality::PeerAccelerator;
      second.locality_declared = true;
      second.bytes = 64;
      second.lines = 1;
      script.push_back(second);
      break;
    }
    case SyntheticPreset::RemoteNumaAccess: {
      SyntheticStep read =
          make_step(EventType::RemoteRead, std::string(kRegionShared), std::string(kProc),
                    std::string(kDomainHost), 24);
      read.direction = AccessDirection::Read;
      read.locality = Locality::RemoteNuma;
      read.locality_declared = true;
      read.bytes = 4096;
      read.pages = 1;
      script.push_back(read);
      SyntheticStep write =
          make_step(EventType::RemoteWrite, std::string(kRegionShared), std::string(kProc),
                    std::string(kDomainHost), 12);
      write.direction = AccessDirection::Write;
      write.state_before = CoherenceState::Shared;
      write.state_after = CoherenceState::Modified;
      write.locality = Locality::RemoteNuma;
      write.locality_declared = true;
      write.bytes = 4096;
      write.pages = 1;
      script.push_back(write);
      break;
    }
    case SyntheticPreset::PingPong: {
      for (int i = 0; i < 6; ++i) {
        SyntheticStep forward =
            make_step(EventType::OwnershipTransfer, std::string(kRegionPing),
                      std::string(kAccA), std::string(kAccB), 1);
        forward.state_before = CoherenceState::Modified;
        forward.state_after = CoherenceState::Invalid;
        forward.locality = Locality::PeerAccelerator;
        forward.locality_declared = true;
        forward.spacing_ns = 20000;
        script.push_back(forward);
        SyntheticStep backward =
            make_step(EventType::OwnershipTransfer, std::string(kRegionPing),
                      std::string(kAccB), std::string(kAccA), 1);
        backward.state_before = CoherenceState::Modified;
        backward.state_after = CoherenceState::Invalid;
        backward.locality = Locality::PeerAccelerator;
        backward.locality_declared = true;
        backward.spacing_ns = 20000;
        script.push_back(backward);
      }
      break;
    }
    case SyntheticPreset::FalseSharingLike: {
      for (int i = 0; i < 8; ++i) {
        SyntheticStep write =
            make_step(EventType::WriteExclusiveTransition, std::string(kRegionLine),
                      std::string(kAccA), std::string(kDomainHost), 1);
        write.direction = AccessDirection::Write;
        write.state_before = CoherenceState::Shared;
        write.state_after = CoherenceState::Exclusive;
        write.granularity = EvidenceGranularity::CacheLine;
        write.locality = Locality::PeerAccelerator;
        write.locality_declared = true;
        script.push_back(write);
        SyntheticStep invalidate =
            make_step(EventType::Invalidation, std::string(kRegionLine), std::string(kAccA),
                      std::string(kAccB), 1);
        invalidate.state_before = CoherenceState::Shared;
        invalidate.state_after = CoherenceState::Invalid;
        invalidate.granularity = EvidenceGranularity::CacheLine;
        invalidate.locality = Locality::PeerAccelerator;
        invalidate.locality_declared = true;
        script.push_back(invalidate);
      }
      break;
    }
    case SyntheticPreset::InvalidationBurst: {
      SyntheticStep burst =
          make_step(EventType::Invalidation, std::string(kRegionShared), std::string(kAccA),
                    std::string(kAccB), 24);
      burst.spacing_ns = 200;
      burst.state_before = CoherenceState::Shared;
      burst.state_after = CoherenceState::Invalid;
      burst.locality = Locality::PeerAccelerator;
      burst.locality_declared = true;
      script.push_back(burst);
      break;
    }
    case SyntheticPreset::CacheToCacheAndWriteback: {
      SyntheticStep transfer =
          make_step(EventType::CacheToCacheTransfer, std::string(kRegionShared),
                    std::string(kAccA), std::string(kAccB), 16);
      transfer.state_before = CoherenceState::SharedClean;
      transfer.state_after = CoherenceState::SharedDirty;
      transfer.locality = Locality::PeerAccelerator;
      transfer.locality_declared = true;
      transfer.bytes = 64;
      transfer.lines = 1;
      script.push_back(transfer);
      SyntheticStep writeback =
          make_step(EventType::Writeback, std::string(kRegionShared), std::string(kAccB),
                    std::string(kDomainHost), 8);
      writeback.state_before = CoherenceState::Modified;
      writeback.state_after = CoherenceState::Shared;
      writeback.bytes = 64;
      writeback.lines = 1;
      script.push_back(writeback);
      break;
    }
    case SyntheticPreset::CxlClassTraffic: {
      SyntheticStep read =
          make_step(EventType::RemoteRead, std::string(kRegionCxl), std::string(kProc),
                    std::string(kDomainCxl), 24);
      read.direction = AccessDirection::Read;
      read.locality = Locality::CxlAttached;
      read.locality_declared = true;
      read.bytes = 4096;
      read.pages = 1;
      script.push_back(read);
      SyntheticStep write =
          make_step(EventType::RemoteWrite, std::string(kRegionCxl), std::string(kProc),
                    std::string(kDomainCxl), 12);
      write.direction = AccessDirection::Write;
      write.locality = Locality::CxlAttached;
      write.locality_declared = true;
      write.bytes = 4096;
      write.pages = 1;
      script.push_back(write);
      SyntheticStep pooled =
          make_step(EventType::MemoryDomainTransfer, std::string(kRegionCxl),
                    std::string(kDomainCxl), std::string(kDomainPooled), 6);
      pooled.locality = Locality::PooledMemory;
      pooled.locality_declared = true;
      pooled.bytes = 4096;
      script.push_back(pooled);
      break;
    }
    case SyntheticPreset::SnoopDirectoryRetry: {
      SyntheticStep snoop =
          make_step(EventType::SnoopRequest, std::string(kRegionShared), std::string(kAccA),
                    std::string(kAccB), 16);
      script.push_back(snoop);
      SyntheticStep miss =
          make_step(EventType::DirectoryMiss, std::string(kRegionShared), std::string(kAccA),
                    std::string(kAccB), 8);
      script.push_back(miss);
      SyntheticStep retry =
          make_step(EventType::CoherenceRetry, std::string(kRegionShared), std::string(kAccA),
                    std::string(kAccB), 8);
      script.push_back(retry);
      SyntheticStep conflict =
          make_step(EventType::CoherenceConflict, std::string(kRegionShared),
                    std::string(kAccA), std::string(kAccB), 4);
      script.push_back(conflict);
      break;
    }
    case SyntheticPreset::AggregateOnlyCounter: {
      SyntheticStep step =
          make_step(EventType::RemoteRead, std::string(kRegionShared), std::string(kProc),
                    std::string(kDomainHost), 1);
      step.granularity = EvidenceGranularity::Device;
      step.precision = Precision::ExactCounterDelta;
      step.backend_event_name = "aggregate-device-counter";
      script.push_back(step);
      break;
    }
    case SyntheticPreset::PageGranularityContention: {
      SyntheticStep page_write =
          make_step(EventType::WriteExclusiveTransition, std::string(kRegionPage),
                    std::string(kProc), std::string(kDomainHost), 12);
      page_write.direction = AccessDirection::Write;
      page_write.granularity = EvidenceGranularity::Page;
      page_write.precision = Precision::SampledEvent;
      page_write.pages = 1;
      page_write.bytes = 4096;
      page_write.locality = Locality::LocalNuma;
      page_write.locality_declared = true;
      script.push_back(page_write);
      SyntheticStep page_invalidate =
          make_step(EventType::Invalidation, std::string(kRegionPage), std::string(kProc),
                    std::string(kAccA), 8);
      page_invalidate.granularity = EvidenceGranularity::Page;
      page_invalidate.precision = Precision::SampledEvent;
      page_invalidate.pages = 1;
      page_invalidate.locality = Locality::LocalNuma;
      page_invalidate.locality_declared = true;
      script.push_back(page_invalidate);
      break;
    }
  }
  return script;
}

SyntheticConfig make_preset_config(SyntheticPreset preset, std::uint64_t seed) {
  SyntheticConfig config;
  config.seed = seed;
  config.resources = base_resources();
  config.regions = base_regions();
  config.script = make_preset_script(preset);
  config.presets.push_back(preset);
  return config;
}

// ---- SyntheticBackend --------------------------------------------------

struct SyntheticBackend::Impl {
  SyntheticConfig config;
  CollectorContext context;
  std::vector<SyntheticStep> expanded;
  std::size_t step_index = 0;
  std::size_t repeat_left = 0;
  std::uint64_t emissions = 0;
  std::uint64_t rejections = 0;
  std::uint64_t counter_samples = 0;
  std::uint64_t counter_raw = 0;
  std::uint64_t counter_generation = 1;
  Nanos clock_ns = 0;
  CoherenceEventId last_event_id;
  EventSequence last_sequence;
  Observation last_observation;
  bool started = false;
  bool malformed_emitted = false;
  std::size_t script_cursor = 0;
  bool region_retired = false;
  bool device_bumped = false;
  bool topology_bumped = false;

  std::uint64_t width_max() const {
    const std::uint32_t width = config.counter_width_bits;
    return width >= 64 ? ~0ull : ((1ull << width) - 1ull);
  }

  ResourceRef ref(ResourceKind kind, const std::string& id, std::uint64_t generation) const {
    ResourceRef reference;
    reference.kind = kind;
    reference.id = ResourceId{id};
    reference.generation = generation;
    return reference;
  }

  /// Derives the resource kind from a synthetic identity prefix.  The backend
  /// must state the real kind: a reference to an accelerator that claims to be
  /// a memory domain resolves to nothing and is correctly reported as stale.
  static ResourceKind kind_of(const std::string& id) noexcept {
    if (id.rfind("md.", 0) == 0) {
      return ResourceKind::MemoryDomain;
    }
    if (id.rfind("cpu.", 0) == 0) {
      return ResourceKind::Processor;
    }
    if (id.rfind("acc.", 0) == 0) {
      return ResourceKind::Accelerator;
    }
    if (id.rfind("cd.", 0) == 0) {
      return ResourceKind::CoherenceDomain;
    }
    if (id.rfind("node.", 0) == 0) {
      return ResourceKind::Node;
    }
    return ResourceKind::Unknown;
  }
};

SyntheticBackend::SyntheticBackend(SyntheticConfig config)
    : impl_(std::make_unique<Impl>()) {
  impl_->config = std::move(config);
}

SyntheticBackend::~SyntheticBackend() { stop(); }

std::string_view SyntheticBackend::name() const noexcept { return "synthetic"; }

std::vector<Capability> SyntheticBackend::capabilities() const {
  std::vector<Capability> capabilities;
  auto add = [&capabilities](std::string key, std::string detail_text) {
    Capability capability;
    capability.key = std::move(key);
    capability.status = CapabilityStatus::Synthetic;
    capability.detail = std::move(detail_text);
    capability.mechanism = "synthetic-exact-scenario";
    capabilities.push_back(std::move(capability));
  };
  add("coherence.cache_line_ownership",
      "simulated per-cache-line ownership transitions with exact ground truth");
  add("coherence.invalidation_counters",
      "simulated invalidation events, individually identified");
  add("accelerator.coherence_telemetry",
      "simulated accelerator cache-line ownership and peer transfers");
  add("memory.cxl_telemetry",
      "simulated CXL-class and pooled-memory coherence traffic");
  add("memory.pooled_telemetry", "simulated pooled-memory domain transfers");
  add("cpu.numa_topology", "simulated multi-domain locality relationships");
  return capabilities;
}

Status SyntheticBackend::set_script(std::vector<SyntheticStep> script) {
  if (impl_->started) {
    return fail(ErrorCode::Busy, "script cannot change after start");
  }
  impl_->config.script = std::move(script);
  return Status();
}

Status SyntheticBackend::start(const CollectorContext& context) {
  if (context.sink == nullptr) {
    return fail(ErrorCode::InvalidArgument, "synthetic backend requires an ingestion sink");
  }
  impl_->context = context;
  // Synthetic time starts at zero so that a seeded run is byte-reproducible.
  impl_->clock_ns = 0;

  std::vector<SyntheticStep> script = impl_->config.script;
  for (SyntheticPreset preset : impl_->config.presets) {
    const std::vector<SyntheticStep> preset_script = make_preset_script(preset);
    script.insert(script.end(), preset_script.begin(), preset_script.end());
  }
  if (script.empty()) {
    return fail(ErrorCode::InvalidArgument, "synthetic script is empty");
  }
  impl_->expanded.reserve(std::min<std::size_t>(kMaxExpandedSteps, script.size() * 32));
  for (const SyntheticStep& step : script) {
    const std::uint32_t repeat = step.repeat == 0 ? 1 : step.repeat;
    for (std::uint32_t i = 0; i < repeat; ++i) {
      if (impl_->expanded.size() >= kMaxExpandedSteps) {
        break;
      }
      impl_->expanded.push_back(step);
    }
  }
  if (impl_->expanded.empty()) {
    return fail(ErrorCode::InvalidArgument, "expanded synthetic script is empty");
  }

  StructureRegistrar* registrar = context.registrar;
  if (registrar != nullptr) {
    NodeRecord node;
    node.id = NodeId{std::string(kNodeId)};
    node.display_name = "synthetic node";
    registrar->register_node(node);

    for (const std::string_view id : {kCoherenceDomain, kCoherenceDomainExact}) {
      CoherenceDomainRecord domain;
      domain.id = CoherenceDomainId{std::string(id)};
      domain.generation = CoherenceDomainGeneration{1};
      domain.protocol_family = "synthetic-exact";
      const Status registered = registrar->register_coherence_domain(domain);
      if (!registered.ok() && registered.code() != ErrorCode::AlreadyExists) {
        return fail(registered.code(),
                    "synthetic coherence domain registration failed: " + registered.describe());
      }
    }

    // Memory domains are registered first: processors, accelerators and
    // regions all reference them, and a reference to an unregistered domain is
    // rejected by the observatory.
    for (int pass = 0; pass < 2; ++pass) {
      for (const SyntheticResourceSpec& spec : impl_->config.resources) {
        const bool domain_pass = spec.kind == ResourceKind::MemoryDomain;
        if ((pass == 0) != domain_pass) {
          continue;
        }
        switch (spec.kind) {
        case ResourceKind::Processor: {
          ProcessorRecord record;
          record.id = ProcessorId{spec.id};
          record.node = NodeId{spec.node};
          record.generation = DeviceGeneration{1};
          record.logical_processor_count = spec.logical_processors;
          record.physical_core_count = spec.logical_processors;
          record.local_memory_domain = MemoryDomainId{std::string(kDomainHost)};
          const Status registered = registrar->register_processor(record);
          if (!registered.ok()) {
            return fail(registered.code(), "synthetic processor registration failed: " +
                                               registered.describe());
          }
          break;
        }
        case ResourceKind::Accelerator: {
          AcceleratorRecord record;
          record.id = AcceleratorId{spec.id};
          record.node = NodeId{spec.node};
          record.generation = DeviceGeneration{1};
          record.vendor = spec.vendor;
          record.vendor_uuid = spec.id + "-uuid";
          record.local_memory_domain =
              MemoryDomainId{spec.id == std::string(kAccA) ? std::string(kDomainAccA)
                                                           : std::string(kDomainAccB)};
          record.peer_coherent_capable = spec.peer_coherent_capable;
          record.peer_coherent_capability_known = true;
          const Status registered = registrar->register_accelerator(record);
          if (!registered.ok()) {
            return fail(registered.code(), "synthetic accelerator registration failed: " +
                                               registered.describe());
          }
          break;
        }
        case ResourceKind::MemoryDomain: {
          MemoryDomainRecord record;
          record.id = MemoryDomainId{spec.id};
          record.generation = MemoryDomainGeneration{1};
          record.node = NodeId{spec.node};
          record.kind = spec.domain_kind;
          record.coherence_domain = CoherenceDomainId{std::string(kCoherenceDomain)};
          record.coherent_with_host = spec.coherent_with_host;
          record.coherent_with_host_known = true;
          record.capacity_bytes = 1ull << 34;
          record.capacity_known = true;
          const Status registered = registrar->register_memory_domain(record);
          if (!registered.ok()) {
            return fail(registered.code(), "synthetic memory domain registration failed: " +
                                               registered.describe());
          }
          break;
        }
        case ResourceKind::Node:
        case ResourceKind::CoherenceDomain:
        case ResourceKind::Unknown:
          break;
        }
      }
    }

    for (const SyntheticRegionSpec& spec : impl_->config.regions) {
      RegionRecord record;
      record.id = MemoryRegionId{spec.id};
      record.generation = MemoryRegionGeneration{1};
      record.owner = spec.owner;
      record.memory_domain = MemoryDomainId{spec.memory_domain};
      record.memory_domain_generation = MemoryDomainGeneration{1};
      record.coherence_domain = CoherenceDomainId{spec.coherence_domain};
      record.sharing_scope = spec.sharing_scope;
      record.size_bytes = spec.size_bytes;
      record.size_known = true;
      record.page_size_bytes = spec.page_size_bytes;
      record.page_size_known = true;
      record.allocation_generation = 1;
      record.mapping_generation = 1;
      record.workload = WorkloadId{spec.workload};
      record.annotation = spec.annotation;
      const Status registered = registrar->register_region(record);
      if (!registered.ok()) {
        return fail(registered.code(), "synthetic region registration failed: " +
                                           registered.describe());
      }
    }

    auto link = [registrar](ResourceRef from, ResourceRef to, Locality locality) {
      TopologyLink topology_link;
      topology_link.from = std::move(from);
      topology_link.to = std::move(to);
      topology_link.locality = locality;
      topology_link.provenance = Provenance::SyntheticBackend;
      registrar->set_topology_link(topology_link);
    };
    link(impl_->ref(ResourceKind::Accelerator, std::string(kAccA), 1),
         impl_->ref(ResourceKind::MemoryDomain, std::string(kDomainHost), 1),
         Locality::LocalAccelerator);
    link(impl_->ref(ResourceKind::Accelerator, std::string(kAccB), 1),
         impl_->ref(ResourceKind::MemoryDomain, std::string(kDomainHost), 1),
         Locality::PeerAccelerator);
    link(impl_->ref(ResourceKind::Processor, std::string(kProc), 1),
         impl_->ref(ResourceKind::MemoryDomain, std::string(kDomainHost), 1),
         Locality::LocalNuma);
    link(impl_->ref(ResourceKind::Processor, std::string(kProc), 1),
         impl_->ref(ResourceKind::MemoryDomain, std::string(kDomainCxl), 1),
         Locality::CxlAttached);
    link(impl_->ref(ResourceKind::Accelerator, std::string(kAccA), 1),
         impl_->ref(ResourceKind::Accelerator, std::string(kAccB), 1),
         Locality::PeerAccelerator);

    for (const Capability& capability : capabilities()) {
      registrar->set_capability(capability);
    }
  }
  impl_->started = true;
  return Status();
}

Status SyntheticBackend::poll(const CollectorContext& context) {
  if (!impl_->started || context.sink == nullptr) {
    return fail(ErrorCode::Busy, "synthetic backend is not started");
  }
  std::size_t budget = context.budget_per_poll == 0 ? 64 : context.budget_per_poll;
  while (budget-- > 0) {
    if (impl_->config.observation_budget != 0 &&
        impl_->emissions >= impl_->config.observation_budget) {
      return Status();
    }
    const SyntheticStep& step = impl_->expanded[impl_->step_index % impl_->expanded.size()];
    const std::uint64_t index = impl_->emissions;

    // ---- structural mutation scenarios --------------------------------
    if (impl_->config.faults.retire_region_enabled &&
        !impl_->region_retired &&
        index >= impl_->config.faults.retire_region_at_index && context.registrar != nullptr) {
      impl_->region_retired = true;
      const std::string target = impl_->config.faults.retire_region_target.empty()
                                     ? std::string(kRegionShared)
                                     : impl_->config.faults.retire_region_target;
      context.registrar->retire_region(MemoryRegionId{target}, MemoryRegionGeneration{1},
                                       "synthetic retirement scenario");
    }
    if (impl_->config.faults.bump_device_generation_enabled && !impl_->device_bumped &&
        index >= impl_->config.faults.bump_device_generation_at_index &&
        context.registrar != nullptr) {
      impl_->device_bumped = true;
      AcceleratorRecord record;
      record.id = AcceleratorId{std::string(kAccA)};
      record.node = NodeId{std::string(kNodeId)};
      record.generation = DeviceGeneration{2};
      record.vendor = "synthetic";
      record.vendor_uuid = std::string(kAccA) + "-uuid";
      record.local_memory_domain = MemoryDomainId{std::string(kDomainAccA)};
      record.peer_coherent_capable = true;
      record.peer_coherent_capability_known = true;
      context.registrar->register_accelerator(record);
    }
    if (impl_->config.faults.bump_topology_enabled && !impl_->topology_bumped &&
        index >= impl_->config.faults.bump_topology_at_index && context.registrar != nullptr) {
      impl_->topology_bumped = true;
      context.registrar->bump_topology("synthetic topology change scenario");
    }

    Observation observation;
    observation.type = step.type;
    observation.timestamp_ns = impl_->clock_ns;
    impl_->clock_ns += step.spacing_ns;
    observation.direction = step.direction;
    observation.state_before = step.state_before;
    observation.state_after = step.state_after;
    observation.bytes = step.bytes;
    observation.lines = step.lines;
    observation.pages = step.pages;
    observation.locality = step.locality;
    observation.locality_declared = step.locality_declared;
    observation.precision = step.precision;
    observation.granularity = step.granularity;
    observation.provenance = step.provenance;
    observation.measured_duration_ns = step.measured_duration_ns;
    observation.topology_generation = context.registrar != nullptr
                                          ? context.registrar->topology_generation()
                                          : TopologyGeneration{1};
    // The backend owns its identity so that fault injection can replay an
    // exact record and can skip sequences deliberately.
    observation.sequence = context.sink->next_sequence();
    observation.event_id = CoherenceEventId{observation.sequence.value()};
    if (!step.region.empty()) {
      observation.region = MemoryRegionId{step.region};
      observation.region_generation = MemoryRegionGeneration{1};
    }
    if (!step.source.empty()) {
      observation.source = impl_->ref(Impl::kind_of(step.source), step.source, 1);
    }
    if (!step.target.empty()) {
      observation.target = impl_->ref(Impl::kind_of(step.target), step.target, 1);
    }
    if (step.force_unknown_event) {
      observation.type = EventType::UnknownCoherenceEvent;
      const Status added = observation.metadata.add(
          "backend.event", step.backend_event_name.empty() ? "unnamed" : step.backend_event_name);
      (void)added;
    }

    // ---- evidence-level fault injection --------------------------------
    const SyntheticFaults& faults = impl_->config.faults;
    const bool stale_generation =
        faults.stale_region_generation_enabled &&
        index == faults.stale_region_generation_at_index && observation.region.has_value();
    if (stale_generation) {
      observation.region_generation = MemoryRegionGeneration{
          observation.region_generation.value().value() + 7};
    }
    if (faults.stale_topology_enabled && index == faults.stale_topology_at_index) {
      observation.topology_generation =
          TopologyGeneration{observation.topology_generation.value() + 3};
    }
    if (faults.emit_malformed_after && index == faults.malformed_at_index) {
      observation.bytes = Limits::kMaxByteCount + 1;
    }
    if (faults.duplicate_enabled && index == faults.duplicate_at_index) {
      // An exact replay of the previous record: same identity, same payload.
      observation = impl_->last_observation;
    } else if (faults.conflicting_duplicate_enabled &&
               index == faults.conflicting_duplicate_at_index) {
      observation = impl_->last_observation;
      observation.bytes = observation.bytes + 4096;
    } else if (faults.stale_replay_enabled && index == faults.stale_replay_at_index) {
      observation.sequence = EventSequence{1};
    }
    if (faults.sequence_gap_size != 0 && index == faults.sequence_gap_at_index) {
      for (std::uint64_t i = 0; i < faults.sequence_gap_size; ++i) {
        context.sink->next_sequence();
      }
    }

    impl_->last_event_id = observation.event_id;
    impl_->last_sequence = observation.sequence;
    const Observation published = observation;
    const Result<IngestionOutcome> outcome = context.sink->publish(std::move(observation));
    impl_->last_observation = published;
    ++impl_->emissions;
    if (!outcome.ok() ||
        outcome.value().disposition == IngestionDisposition::Rejected) {
      ++impl_->rejections;
    }
    ++impl_->step_index;

    if (impl_->config.publish_counters) {
      CounterPublication publication;
      publication.counter = CounterId{impl_->config.counter_id};
      publication.mapped_type = impl_->config.counter_mapped_type;
      publication.kind = impl_->config.counter_kind;
      publication.scope = impl_->config.counter_scope;
      publication.width_bits = impl_->config.counter_width_bits;
      publication.generation = CounterGeneration{impl_->counter_generation};
      publication.sampling_epoch = SamplingEpoch{1};
      publication.bytes_per_unit = impl_->config.counter_bytes_per_unit;
      publication.timestamp_ns = impl_->clock_ns;
      publication.provenance = Provenance::SyntheticBackend;
      publication.granularity = impl_->config.counter_scope == CounterScope::PerRegion
                                    ? EvidenceGranularity::Region
                                    : EvidenceGranularity::Device;
      publication.source = impl_->ref(ResourceKind::Accelerator, std::string(kAccA), 1);
      publication.topology_generation = observation.topology_generation;

      const std::uint64_t sample = impl_->counter_samples;
      const std::uint64_t width_max = impl_->width_max();
      if (faults.counter_reset_after_samples != 0 &&
          sample >= faults.counter_reset_after_samples && sample % 7 == 0) {
        impl_->counter_raw = 0;
      }
      impl_->counter_raw = (impl_->counter_raw + impl_->config.counter_increment) %
                           (width_max == ~0ull ? width_max : width_max + 1);
      if (faults.counter_wrap_after_samples != 0 &&
          sample == faults.counter_wrap_after_samples) {
        impl_->counter_raw = width_max - 4;
        publication.kind = CounterKind::Wrapping;
      }
      if (faults.counter_generation_change_after_samples != 0 &&
          sample == faults.counter_generation_change_after_samples) {
        impl_->counter_generation += 1;
        publication.generation = CounterGeneration{impl_->counter_generation};
      }
      publication.raw_value = impl_->counter_raw;
      const Result<CounterOutcome> counter_outcome =
          context.sink->publish_counter(std::move(publication));
      if (!counter_outcome.ok()) {
        ++impl_->rejections;
      }
      ++impl_->counter_samples;
    }
  }
  return Status();
}

Status SyntheticBackend::stop() {
  impl_->started = false;
  return Status();
}

std::uint64_t SyntheticBackend::emissions() const noexcept { return impl_->emissions; }
std::uint64_t SyntheticBackend::rejections() const noexcept { return impl_->rejections; }
CoherenceEventId SyntheticBackend::last_event_id() const noexcept {
  return impl_->last_event_id;
}
EventSequence SyntheticBackend::last_sequence() const noexcept { return impl_->last_sequence; }

}  // namespace sol::coherence