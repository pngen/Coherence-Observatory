// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Deterministic synthetic coherence backend.
//
// The synthetic backend emits exact scenarios whose ground truth is known by
// construction.  It publishes through the same ingestion, validation,
// aggregation, attribution and analysis pipeline as every real backend: there
// is no test-only alternate implementation.

#ifndef COHERENCE_BACKENDS_SYNTHETIC_HPP
#define COHERENCE_BACKENDS_SYNTHETIC_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "coherence/backend.hpp"
#include "coherence/observation.hpp"

namespace sol::coherence {

/// Declaration of one synthetic resource.
struct COHERENCE_API SyntheticResourceSpec {
  ResourceKind kind = ResourceKind::MemoryDomain;
  std::string id;
  std::string node;
  std::string display_name;
  /// Memory domain fields.
  MemoryDomainKind domain_kind = MemoryDomainKind::HostDram;
  std::string coherence_domain;
  bool coherent_with_host = true;
  std::uint64_t capacity_bytes = 0;
  /// Accelerator fields.
  std::string vendor;
  bool peer_coherent_capable = false;
  /// Processor fields.
  std::uint32_t logical_processors = 0;
};

/// Declaration of one synthetic memory region.
struct COHERENCE_API SyntheticRegionSpec {
  std::string id;
  std::string memory_domain;
  std::string coherence_domain;
  std::string owner = "synthetic.workload";
  SharingScope sharing_scope = SharingScope::DeviceShared;
  std::uint64_t size_bytes = 4096;
  std::uint64_t page_size_bytes = 4096;
  std::string workload;
  std::string annotation;
};

/// One emission in a synthetic script.
struct COHERENCE_API SyntheticStep {
  EventType type = EventType::RemoteRead;
  std::string region;
  std::string source;
  std::string target;
  AccessDirection direction = AccessDirection::None;
  CoherenceState state_before = CoherenceState::Unknown;
  CoherenceState state_after = CoherenceState::Unknown;
  std::uint64_t bytes = 0;
  std::uint64_t lines = 0;
  std::uint64_t pages = 0;
  Locality locality = Locality::Unknown;
  bool locality_declared = false;
  Precision precision = Precision::ExactEvent;
  EvidenceGranularity granularity = EvidenceGranularity::Region;
  Provenance provenance = Provenance::SyntheticBackend;
  std::int64_t measured_duration_ns = -1;
  /// Nanoseconds between consecutive emissions of this step.
  std::int64_t spacing_ns = 1000;
  /// Number of times to repeat this step.
  std::uint32_t repeat = 1;
  /// Overrides for building an unmappable backend-native event.
  bool force_unknown_event = false;
  std::string backend_event_name;
};

/// Preset scripts covering the scenarios the runtime must explain.
enum class SyntheticPreset : std::uint8_t {
  /// Steady shared reads across two accelerators.
  SteadySharedReads = 0,
  /// Remote reads and writes across a NUMA boundary.
  RemoteNumaAccess = 1,
  /// Alternating ownership between two accelerators.
  PingPong = 2,
  /// Line-granularity writes to distinct offsets in one line.
  FalseSharingLike = 3,
  /// A dense burst of invalidations.
  InvalidationBurst = 4,
  /// Cache-to-cache transfers and writebacks.
  CacheToCacheAndWriteback = 5,
  /// CXL-class and pooled-memory traffic.
  CxlClassTraffic = 6,
  /// Snoop/directory/retry traffic.
  SnoopDirectoryRetry = 7,
  /// Aggregate-only counter evidence with no region identity.
  AggregateOnlyCounter = 8,
  /// Evidence that resolves a page but not a line.
  PageGranularityContention = 9,
};

COHERENCE_API std::string_view to_string(SyntheticPreset preset) noexcept;
COHERENCE_API Result<SyntheticPreset> parse_synthetic_preset(std::string_view text);

/// Builds the standard script for a preset.
COHERENCE_API std::vector<SyntheticStep> make_preset_script(SyntheticPreset preset);

/// Deterministic fault injection for the synthetic backend.
struct COHERENCE_API SyntheticFaults {
  /// Skip this many sequences at the given emission index, creating a gap.
  std::uint64_t sequence_gap_at_index = 0;
  std::uint64_t sequence_gap_size = 0;
  /// Re-send the observation at this index (duplicate detection).
  std::uint64_t duplicate_at_index = 0;
  bool duplicate_enabled = false;
  /// Send a conflicting duplicate: same event id, different payload.
  std::uint64_t conflicting_duplicate_at_index = 0;
  bool conflicting_duplicate_enabled = false;
  /// Re-send an old sequence after the watermark advanced (stale replay).
  std::uint64_t stale_replay_at_index = 0;
  bool stale_replay_enabled = false;
  /// Emit a region generation that no longer matches the registration.
  std::uint64_t stale_region_generation_at_index = 0;
  bool stale_region_generation_enabled = false;
  /// Emit evidence bound to a superseded topology generation.
  std::uint64_t stale_topology_at_index = 0;
  bool stale_topology_enabled = false;
  /// Emit the synthetic counter reset scenario after this many counter polls.
  std::uint64_t counter_reset_after_samples = 0;
  /// Emit a counter wrap after this many counter polls.
  std::uint64_t counter_wrap_after_samples = 0;
  /// Emit a counter generation change after this many counter polls.
  std::uint64_t counter_generation_change_after_samples = 0;
  /// Retire a region after this emission index (0 disables).
  std::uint64_t retire_region_at_index = 0;
  bool retire_region_enabled = false;
  std::string retire_region_target;
  /// Mutate a device generation after this emission index.
  std::uint64_t bump_device_generation_at_index = 0;
  bool bump_device_generation_enabled = false;
  /// Bump the topology generation after this emission index.
  std::uint64_t bump_topology_at_index = 0;
  bool bump_topology_enabled = false;
  /// Emit structurally invalid observations (must be rejected).
  bool emit_malformed_after = false;
  std::uint64_t malformed_at_index = 0;
};

/// Synthetic backend configuration.
struct COHERENCE_API SyntheticConfig {
  std::uint64_t seed = 0x5EEDC0FFEEull;
  std::string publisher_id = "pub.synthetic.1";
  std::string publisher_name = "synthetic-collector";
  std::string observer_id = "observer.local";
  std::string node_id = "node.synthetic.0";

  std::vector<SyntheticResourceSpec> resources;
  std::vector<SyntheticRegionSpec> regions;
  std::vector<SyntheticStep> script;
  /// Presets appended after an explicit script.
  std::vector<SyntheticPreset> presets;

  /// Stop after this many emissions; 0 means run the script once and idle.
  std::uint64_t observation_budget = 0;
  /// Emissions per poll().
  std::size_t emissions_per_poll = 64;
  /// Publish counters in addition to events.
  bool publish_counters = false;
  std::string counter_id = "ctr.coherence.traffic";
  EventType counter_mapped_type = EventType::RemoteRead;
  CounterScope counter_scope = CounterScope::PerDevice;
  CounterKind counter_kind = CounterKind::Absolute;
  std::uint32_t counter_width_bits = 64;
  std::uint64_t counter_increment = 977;
  std::uint64_t counter_bytes_per_unit = 64;
  SyntheticFaults faults;
};

/// Deterministic synthetic coherence collector.
class COHERENCE_API SyntheticBackend : public Collector {
 public:
  explicit SyntheticBackend(SyntheticConfig config = {});
  ~SyntheticBackend() override;

  SyntheticBackend(const SyntheticBackend&) = delete;
  SyntheticBackend& operator=(const SyntheticBackend&) = delete;

  std::string_view name() const noexcept override;
  std::vector<Capability> capabilities() const override;
  Status start(const CollectorContext& context) override;
  Status poll(const CollectorContext& context) override;
  Status stop() override;

  /// Number of emissions published so far.
  std::uint64_t emissions() const noexcept;
  /// Observations the sink rejected.
  std::uint64_t rejections() const noexcept;

  /// Replaces the script.  Only valid before start().
  Status set_script(std::vector<SyntheticStep> script);

  /// Event id of the last published observation (for replay tests).
  CoherenceEventId last_event_id() const noexcept;
  /// Sequence of the last published observation.
  EventSequence last_sequence() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Returns a configuration for p preset with a small, deterministic topology.
COHERENCE_API SyntheticConfig make_preset_config(SyntheticPreset preset,
                                                 std::uint64_t seed = 0x5EEDC0FFEEull);

}  // namespace sol::coherence

#endif  // COHERENCE_BACKENDS_SYNTHETIC_HPP
