// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Findings: explainable, decomposed analysis results.
//
// Every finding names its reasons and the metrics that triggered them.  The
// runtime never emits an opaque score: a finding without a decomposition is a
// defect.

#ifndef COHERENCE_FINDINGS_HPP
#define COHERENCE_FINDINGS_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "coherence/attribution.hpp"
#include "coherence/cost.hpp"
#include "coherence/locality.hpp"
#include "coherence/observation.hpp"
#include "coherence/precision.hpp"
#include "coherence/provenance.hpp"
#include "coherence/region.hpp"
#include "coherence/taxonomy.hpp"

namespace sol::coherence {

/// Stable finding classes.
enum class FindingKind : std::uint8_t {
  Hotspot = 0,
  PingPong = 1,
  FalseSharingLike = 2,
  InvalidationBurst = 3,
  RemoteAccessConcentration = 4,
  CounterDiscontinuity = 5,
  SequenceGap = 6,
  EvidenceLoss = 7,
  StaleEvidence = 8,
  CapacityPressure = 9,
  PublisherLoss = 10,
  UnsupportedObservability = 11,
};

COHERENCE_API std::string_view to_string(FindingKind kind) noexcept;
COHERENCE_API Result<FindingKind> parse_finding_kind(std::string_view text);

/// Named reason codes used by analyzers.
namespace finding_reason {
inline constexpr std::string_view kHighRemoteReadRate = "remote_read_rate";
inline constexpr std::string_view kHighRemoteWriteRate = "remote_write_rate";
inline constexpr std::string_view kRepeatedOwnershipTransfer = "ownership_transfer_count";
inline constexpr std::string_view kInvalidationBurst = "invalidation_burst";
inline constexpr std::string_view kCrossNumaTraffic = "cross_numa_bytes";
inline constexpr std::string_view kCrossDeviceTraffic = "cross_device_bytes";
inline constexpr std::string_view kCrossDomainTraffic = "cross_domain_bytes";
inline constexpr std::string_view kCxlClassTraffic = "cxl_class_bytes";
inline constexpr std::string_view kDisproportionateCoherenceBytes =
    "coherence_byte_share";
inline constexpr std::string_view kRegionLevelContention = "region_contention";
inline constexpr std::string_view kSourceTargetConcentration =
    "source_target_concentration";
inline constexpr std::string_view kAlternatingOwnership = "alternating_ownership";
inline constexpr std::string_view kDistinctWritersPerLine = "distinct_writers_per_line";
inline constexpr std::string_view kSharedReadWithInvalidations =
    "shared_read_with_invalidations";
inline constexpr std::string_view kCounterReset = "counter_reset";
inline constexpr std::string_view kCounterWrap = "counter_wrap";
inline constexpr std::string_view kCounterGenerationChange = "counter_generation_change";
inline constexpr std::string_view kMissingSequence = "missing_sequence";
inline constexpr std::string_view kDuplicateRejected = "duplicate_rejected";
inline constexpr std::string_view kAggregateSaturation = "aggregate_saturation";
inline constexpr std::string_view kAggregateKeyPressure = "aggregate_key_pressure";
inline constexpr std::string_view kJournalEviction = "journal_eviction";
inline constexpr std::string_view kPublisherFenced = "publisher_fenced";
inline constexpr std::string_view kNoCapability = "capability_unsupported";
inline constexpr std::string_view kInsufficientPrecision = "insufficient_precision";
inline constexpr std::string_view kInsufficientGranularity = "insufficient_granularity";
inline constexpr std::string_view kStaleGenerations = "stale_generation_evidence";
}  // namespace finding_reason

/// One decomposed metric supporting a finding.
struct COHERENCE_API FindingMetric {
  std::string name;
  double value = 0.0;
  std::string unit;
  /// Threshold the metric was compared against; NaN when informational.
  double threshold = 0.0;
  bool has_threshold = false;
  bool exceeded = false;
};

/// How strongly false-sharing-like behaviour is supported.
enum class ContentionClass : std::uint8_t {
  None = 0,
  /// Line-granularity evidence with multiple writers to the same line.
  CacheLineFalseSharingSupported = 1,
  /// Region-granularity evidence showing contention.
  RegionContentionLikely = 2,
  /// Page-granularity evidence only.
  PageLevelContention = 3,
  /// Evidence is too coarse to say anything.
  InsufficientGranularity = 4,
};

COHERENCE_API std::string_view to_string(ContentionClass value) noexcept;

/// A complete, explainable finding.
struct COHERENCE_API Finding {
  FindingId id;
  FindingKind kind = FindingKind::Hotspot;

  /// Subject kind, e.g. "region", "device", "publisher", "system".
  std::string subject_kind;
  /// Subject identity text.
  std::string subject;

  /// Named reasons; sorted and de-duplicated for deterministic output.
  std::vector<std::string> reasons;
  /// Decomposed supporting metrics; sorted by name.
  std::vector<FindingMetric> metrics;
  /// Bounded supporting evidence.
  std::vector<EvidenceRef> evidence;
  /// Facts the evidence could not supply.
  std::vector<std::string> missing_evidence;
  /// Unresolved alternatives, when the finding is qualified.
  std::vector<std::string> ambiguity;

  Precision precision = Precision::Unknown;
  Provenance provenance = Provenance::Unknown;
  Reality reality = Reality::Real;

  GenerationBindings bindings;

  CostEstimate cost;

  // ---- Kind-specific payload ------------------------------------------
  /// Ping-pong participants (bounded, sorted).
  std::vector<std::string> participants;
  /// Alternation count for ping-pong findings.
  std::uint64_t alternations = 0;
  /// Compressed direction sequence, e.g. "A>B B>A A>B".
  std::string direction_sequence;
  /// Contention classification for false-sharing-like findings.
  ContentionClass contention = ContentionClass::None;
  /// Granularity of the evidence behind the finding.
  EvidenceGranularity granularity = EvidenceGranularity::Unknown;
  /// Locality class of the subject when established.
  Locality locality = Locality::Unknown;
  bool locality_established = false;

  /// True when the finding came from restored historical evidence.
  bool historical = false;

  /// Deterministic ordering key.
  std::string ordering_key() const;
};

/// Thresholds for hotspot analysis.  Every threshold is named data, not code.
struct COHERENCE_API HotspotPolicy {
  std::uint32_t version = 1;
  double min_remote_read_rate_per_second = 1000.0;
  double min_remote_write_rate_per_second = 500.0;
  std::uint64_t min_remote_accesses = 64;
  std::uint64_t min_ownership_transfers = 16;
  std::uint64_t min_invalidations = 32;
  std::uint64_t min_region_events = 64;
  std::uint64_t min_cross_numa_bytes = 1u << 20;
  std::uint64_t min_cross_device_bytes = 1u << 20;
  std::uint64_t min_cross_domain_bytes = 1u << 20;
  std::uint64_t min_cxl_class_bytes = 1u << 20;
  double min_coherence_byte_share = 0.25;
  double min_source_target_concentration = 0.80;
  std::size_t max_hotspots = 64;
};

/// Ping-pong detection policy.
struct COHERENCE_API PingPongPolicy {
  std::uint32_t version = 1;
  /// Minimum number of direction alternations to report a pattern.
  std::uint32_t min_alternations = 4;
  /// Window over which alternations must occur.
  std::int64_t window_ns = 1000000000;  // 1 second
  /// Maximum gap between consecutive transfers for them to belong together.
  std::int64_t max_gap_ns = 100000000;  // 100 ms
  /// Minimum precision of the underlying evidence.
  Precision min_precision = Precision::ExactEvent;
  /// Minimum number of distinct participants.
  std::uint32_t min_participants = 2;
  std::size_t max_findings = 64;
};

/// False-sharing-like detection policy.
struct COHERENCE_API FalseSharingPolicy {
  std::uint32_t version = 1;
  std::uint32_t min_distinct_writers = 2;
  std::uint64_t min_invalidations = 4;
  std::uint64_t min_shared_reads = 2;
  std::int64_t window_ns = 1000000000;
  Precision min_precision = Precision::ExactEvent;
  std::size_t max_findings = 64;
};

/// Invalidation analysis policy.
struct COHERENCE_API InvalidationPolicy {
  std::uint32_t version = 1;
  /// A burst is at least this many invalidations...
  std::uint64_t burst_threshold = 8;
  /// ...within this window.
  std::int64_t burst_window_ns = 1000000;  // 1 ms
  std::size_t max_findings = 64;
};

/// Remote-access analysis policy.
struct COHERENCE_API RemoteAccessPolicy {
  std::uint32_t version = 1;
  std::size_t max_rows = 256;
  /// Concentration above this share of remote traffic is reported.
  double concentration_threshold = 0.5;
  std::size_t max_findings = 32;
};

/// One row of remote-access analysis: a source->target pair over one region.
struct COHERENCE_API RemoteAccessRow {
  std::string source;
  std::string target;
  MemoryRegionId region;
  MemoryRegionGeneration region_generation;
  AccessDirection direction = AccessDirection::None;
  Locality locality = Locality::Unknown;
  bool locality_established = false;
  std::uint64_t accesses = 0;
  std::uint64_t bytes = 0;
  std::uint64_t read_accesses = 0;
  std::uint64_t write_accesses = 0;
  CostEstimate cost;
  TopologyGeneration topology_generation;
  Precision precision = Precision::Unknown;
  Provenance provenance = Provenance::Unknown;
  Reality reality = Reality::Real;
  bool aggregate_only = false;
};

/// One row of invalidation analysis.
struct COHERENCE_API InvalidationRow {
  std::string source;
  std::string target;
  MemoryRegionId region;
  MemoryRegionGeneration region_generation;
  CoherenceState state_before = CoherenceState::Unknown;
  CoherenceState state_after = CoherenceState::Unknown;
  std::uint64_t individual_events = 0;
  std::uint64_t aggregate_counter_invalidations = 0;
  std::uint64_t burst_count = 0;
  std::uint64_t max_burst_size = 0;
  std::uint64_t bytes_invalidated = 0;
  CostEstimate cost;
  EvidenceGranularity granularity = EvidenceGranularity::Unknown;
  Precision precision = Precision::Unknown;
  Provenance provenance = Provenance::Unknown;
  Reality reality = Reality::Real;
};

/// Complete analysis of one memory region.
struct COHERENCE_API RegionAnalysis {
  MemoryRegionId region;
  MemoryRegionGeneration generation;
  bool registered = false;
  bool retired = false;
  std::string owner;
  SharingScope sharing_scope = SharingScope::Unknown;
  MemoryDomainId memory_domain;

  std::uint64_t observations = 0;
  std::uint64_t bytes = 0;
  std::uint64_t remote_accesses = 0;
  std::uint64_t invalidations = 0;
  std::uint64_t ownership_transfers = 0;

  std::vector<Finding> findings;
  std::vector<EvidenceRef> evidence;
  std::vector<std::string> missing_evidence;

  Precision precision = Precision::Unknown;
  Provenance provenance = Provenance::Unknown;
  Reality reality = Reality::Real;
  GenerationBindings bindings;
  CostEstimate cost;
  bool historical = false;
};

}  // namespace sol::coherence

#endif  // COHERENCE_FINDINGS_HPP
