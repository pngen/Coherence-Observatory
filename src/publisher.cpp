// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/publisher.hpp"

#include "coherence/checked.hpp"

namespace sol::coherence {

std::string_view to_string(FenceReason reason) noexcept {
  switch (reason) {
    case FenceReason::NotFenced: return "NOT_FENCED";
    case FenceReason::ConnectionClosed: return "CONNECTION_CLOSED";
    case FenceReason::Administrative: return "ADMINISTRATIVE";
    case FenceReason::LivenessDeadline: return "LIVENESS_DEADLINE";
    case FenceReason::CoordinatorRestart: return "COORDINATOR_RESTART";
    case FenceReason::SupersededByNewBoot: return "SUPERSEDED_BY_NEW_BOOT";
  }
  return "NOT_FENCED";
}

std::string_view to_string(StaleEvidenceRecord::Kind kind) noexcept {
  switch (kind) {
    case StaleEvidenceRecord::Kind::StaleGeneration: return "STALE_GENERATION";
    case StaleEvidenceRecord::Kind::RetiredRegion: return "RETIRED_REGION";
    case StaleEvidenceRecord::Kind::TopologySuperseded: return "TOPOLOGY_SUPERSEDED";
    case StaleEvidenceRecord::Kind::StaleBoot: return "STALE_BOOT";
    case StaleEvidenceRecord::Kind::StaleEpoch: return "STALE_EPOCH";
    case StaleEvidenceRecord::Kind::StaleSequence: return "STALE_SEQUENCE";
    case StaleEvidenceRecord::Kind::FencedPublisher: return "FENCED_PUBLISHER";
  }
  return "STALE_GENERATION";
}

std::uint64_t LossReport::total_rejected() const noexcept {
  std::uint64_t total = 0;
  const std::uint64_t values[] = {
      rejected_observations,       rejected_duplicates,
      rejected_conflicting_duplicates, rejected_stale_sequences,
      rejected_unauthorized,       rejected_malformed,
      rejected_overflow,           dropped_aggregate_updates,
      dropped_journal_entries,     dropped_region_samples,
      evicted_history_records,     missing_sequences,
      backpressure_rejections};
  for (std::uint64_t value : values) {
    const AddResult r = checked_add(total, value);
    total = r.value;
  }
  return total;
}

}  // namespace sol::coherence
