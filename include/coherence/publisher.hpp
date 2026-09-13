// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#ifndef COHERENCE_PUBLISHER_HPP
#define COHERENCE_PUBLISHER_HPP

#include <cstdint>
#include <string>

#include "coherence/ids.hpp"
#include "coherence/provenance.hpp"
#include "coherence/taxonomy.hpp"
#include "coherence/time.hpp"

namespace sol::coherence {

/// Why a publisher boot identity lost authority.
enum class FenceReason : std::uint8_t {
  NotFenced = 0,
  /// The publisher's transport connection closed.
  ConnectionClosed = 1,
  /// Administrative fence.
  Administrative = 2,
  /// The publisher missed its liveness deadline.
  LivenessDeadline = 3,
  /// The coordinator restarted; every prior boot lost dynamic authority.
  CoordinatorRestart = 4,
  /// A replacement boot registered the same publisher identity.
  SupersededByNewBoot = 5,
};

COHERENCE_API std::string_view to_string(FenceReason reason) noexcept;

/// Generates a fresh, process-unique publisher boot identity.
///
/// A replacement publisher process must present a new value: a fenced boot
/// identity is never re-admitted.
COHERENCE_API PublisherBootId make_publisher_boot_id() noexcept;

/// Sequence accounting for one publisher boot.
struct COHERENCE_API SequenceState {
  /// Highest sequence accepted as contiguous-or-ahead.
  EventSequence high_watermark;
  /// Next exactly-expected sequence.
  EventSequence next_expected;
  std::uint64_t accepted = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t conflicting_duplicates = 0;
  std::uint64_t late_events = 0;
  std::uint64_t missing_events = 0;
  std::uint64_t rejected_stale = 0;
  std::uint64_t counter_resets = 0;
  std::uint64_t counter_wraps = 0;
  std::uint64_t sequence_gaps = 0;
};

/// Public, immutable view of a publisher registration.
struct COHERENCE_API PublisherView {
  PublisherId id;
  PublisherBootId boot;
  ObserverId observer;
  NodeId node;
  std::string display_name;
  Provenance provenance = Provenance::Unknown;
  Reality reality = Reality::Real;

  /// Coordinator epoch under which this boot registered.
  CoordinatorEpoch registered_epoch;
  EvidenceGeneration evidence_generation;
  SamplingEpoch sampling_epoch;

  Nanos registered_at_ns = 0;
  Nanos last_seen_ns = 0;

  /// Dynamic liveness.  False after any restart and after fencing; a publisher
  /// is only current again after a fresh registration under a new boot.
  bool current = false;
  bool fenced = false;
  FenceReason fence_reason = FenceReason::NotFenced;
  std::string fence_detail;

  /// True when this record was restored from durable state rather than
  /// observed in this coordinator incarnation.
  bool loaded_from_state = false;

  SequenceState sequences;
  std::uint64_t accepted_events = 0;
  std::uint64_t accepted_batches = 0;
  std::uint64_t accepted_counters = 0;
  std::uint64_t rejected_events = 0;
};

/// Evidence that was rejected, superseded or otherwise not current.
struct COHERENCE_API StaleEvidenceRecord {
  enum class Kind : std::uint8_t {
    StaleGeneration = 0,
    RetiredRegion = 1,
    TopologySuperseded = 2,
    StaleBoot = 3,
    StaleEpoch = 4,
    StaleSequence = 5,
    FencedPublisher = 6,
  };

  Kind kind = Kind::StaleGeneration;
  CoherenceEventId event_id;
  PublisherId publisher;
  PublisherBootId publisher_boot;
  MemoryRegionId region;
  EventType event_type = EventType::UnknownCoherenceEvent;
  Nanos observed_at_ns = 0;
  std::string detail;
};

COHERENCE_API std::string_view to_string(StaleEvidenceRecord::Kind kind) noexcept;

/// Explicit evidence-loss accounting.  Loss is never silent.
struct COHERENCE_API LossReport {
  std::uint64_t rejected_observations = 0;
  std::uint64_t rejected_duplicates = 0;
  std::uint64_t rejected_conflicting_duplicates = 0;
  std::uint64_t rejected_stale_sequences = 0;
  std::uint64_t rejected_unauthorized = 0;
  std::uint64_t rejected_malformed = 0;
  std::uint64_t rejected_overflow = 0;
  std::uint64_t dropped_aggregate_updates = 0;
  std::uint64_t dropped_journal_entries = 0;
  std::uint64_t dropped_region_samples = 0;
  std::uint64_t evicted_history_records = 0;
  std::uint64_t missing_sequences = 0;
  std::uint64_t backpressure_rejections = 0;

  /// Total observations that were offered and not accepted.
  std::uint64_t total_rejected() const noexcept;

  bool any() const noexcept { return total_rejected() != 0; }
};

}  // namespace sol::coherence

#endif  // COHERENCE_PUBLISHER_HPP
