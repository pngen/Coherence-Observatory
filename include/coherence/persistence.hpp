// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Durable structural state.
//
// Only state that remains meaningful after a restart is persisted, and nothing
// dynamic ever returns as current.  The format is versioned, length-bounded
// and integrity-checked; a corrupt or truncated file is rejected completely
// and never partially applied.

#ifndef COHERENCE_PERSISTENCE_HPP
#define COHERENCE_PERSISTENCE_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "coherence/error.hpp"
#include "coherence/export.hpp"
#include "coherence/ids.hpp"
#include "coherence/time.hpp"

namespace sol::coherence {

/// What a save or load actually moved.
struct COHERENCE_API PersistenceReport {
  std::uint64_t bytes_written = 0;
  std::uint64_t bytes_read = 0;
  std::uint64_t sections = 0;
  std::uint64_t regions = 0;
  std::uint64_t processors = 0;
  std::uint64_t accelerators = 0;
  std::uint64_t memory_domains = 0;
  std::uint64_t coherence_domains = 0;
  std::uint64_t topology_links = 0;
  std::uint64_t publisher_watermarks = 0;
  std::uint64_t history_records = 0;
  std::uint64_t history_aggregates = 0;
  /// Coordinator epoch the saved state was produced under.
  CoordinatorEpoch source_coordinator_epoch;
  /// Coordinator epoch that will be used by the loading incarnation.
  CoordinatorEpoch next_coordinator_epoch;
  bool atomic_replacement = true;
};

/// Options for save and load.
struct COHERENCE_API PersistenceOptions {
  /// Maximum bytes accepted from a state file.  Files larger than this are
  /// rejected before any allocation proportional to their size.
  std::uint64_t max_bytes = Limits::kMaxPersistenceBytes;
  /// Persist bounded historical aggregates and evidence summaries.
  bool include_history = true;
  /// Write through a temporary file and replace atomically.
  bool atomic_replace = true;
  /// Bounded history limits honoured on save.
  std::size_t max_history_records = Limits::kMaxHistoryRecords;
};

/// Returns the canonical state-file magic ("COBSST01").
COHERENCE_API std::string_view state_magic() noexcept;

}  // namespace sol::coherence

#endif  // COHERENCE_PERSISTENCE_HPP
