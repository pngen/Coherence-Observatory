// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Every externally-reachable resource is bounded here.  No untrusted length or
// count reaches an allocation without first being checked against these limits.

#ifndef COHERENCE_LIMITS_HPP
#define COHERENCE_LIMITS_HPP

#include <cstddef>
#include <cstdint>

namespace sol::coherence {

/// Compile-time resource bounds for the runtime.
///
/// The values are deliberately conservative: they bound worst-case memory use
/// of a hostile or merely broken publisher, and they are reported verbatim in
/// error messages so operators can reason about rejections.
struct Limits {
  // ---- Identity and registration -------------------------------------
  static constexpr std::size_t kMaxNameLength = 64;
  static constexpr std::size_t kMaxPublisherCount = 256;
  static constexpr std::size_t kMaxObserverCount = 16;
  static constexpr std::size_t kMaxNodeCount = 64;
  static constexpr std::size_t kMaxProcessorCount = 1024;
  static constexpr std::size_t kMaxAcceleratorCount = 256;
  static constexpr std::size_t kMaxMemoryDomainCount = 256;
  static constexpr std::size_t kMaxCoherenceDomainCount = 256;
  static constexpr std::size_t kMaxRegionCount = 100000;
  static constexpr std::size_t kMaxRegionAnnotationLength = 96;
  static constexpr std::size_t kMaxTopologyLinkCount = 4096;
  static constexpr std::size_t kMaxTopologyNodeCount = 64;

  // ---- Ingestion ------------------------------------------------------
  static constexpr std::size_t kMaxBatchEvents = 1024;
  static constexpr std::size_t kMaxMetadataEntries = 8;
  static constexpr std::size_t kMaxMetadataKeyLength = 48;
  static constexpr std::size_t kMaxMetadataValueLength = 160;
  static constexpr std::size_t kMaxPendingIngestion = 1u << 14;
  static constexpr std::size_t kMaxSequenceWindow = 4096;
  static constexpr std::size_t kMaxRecentEventIds = 4096;

  // ---- Wire protocol --------------------------------------------------
  static constexpr std::size_t kFrameHeaderSize = 32;
  static constexpr std::size_t kFrameTrailerSize = 4;
  static constexpr std::size_t kMaxFramePayload = 1u << 20;   // 1 MiB
  static constexpr std::size_t kMaxConnections = 64;
  static constexpr std::size_t kMaxFrameWriteQueue = 64;

  // ---- Analysis and reporting ----------------------------------------
  static constexpr std::size_t kMaxAggregateKeys = 1u << 16;
  static constexpr std::size_t kMaxFindings = 4096;
  static constexpr std::size_t kMaxQueryResults = 4096;
  static constexpr std::size_t kMaxTimeBuckets = 4096;
  static constexpr std::size_t kMaxAttributionCandidates = 16;
  static constexpr std::size_t kMaxExplanationLines = 512;
  static constexpr std::size_t kMaxEvidenceRefs = 32;
  static constexpr std::size_t kMaxAttributionTargets = 16;

  // ---- Evidence retention --------------------------------------------
  static constexpr std::size_t kMaxObservationJournal = 1u << 14;
  static constexpr std::size_t kMaxTrackedRegions = 8192;
  static constexpr std::size_t kMaxRegionSamples = 256;
  static constexpr std::size_t kMaxCounterRecords = 4096;
  static constexpr std::size_t kMaxHistoryRecords = 1u << 13;
  static constexpr std::size_t kMaxHistoryAggregates = 4096;
  static constexpr std::size_t kMaxPublisherWatermarks = 256;

  // ---- Persistence ----------------------------------------------------
  static constexpr std::uint64_t kMaxPersistenceBytes = 256ull << 20;
  static constexpr std::uint32_t kStateVersion = 1;
  static constexpr std::size_t kStateHeaderSize = 64;
  static constexpr std::size_t kStateSectionHeaderSize = 16;
  static constexpr std::size_t kStateTrailerSize = 16;

  // ---- Threading ------------------------------------------------------
  static constexpr std::size_t kMaxThreads = 8;

  // ---- Numeric guards -------------------------------------------------
  static constexpr std::uint64_t kMaxByteCount = 1ull << 48;        // 256 TiB
  static constexpr std::uint64_t kMaxLineCount = 1ull << 40;
  static constexpr std::uint64_t kMaxCounterWidth = 64;
  static constexpr std::uint64_t kMaxTimestampNs = 1ll << 62;
  static constexpr std::int64_t kMaxMeasuredDurationNs = 1ll << 42;  // ~73 minutes
};

}  // namespace sol::coherence

#endif  // COHERENCE_LIMITS_HPP
