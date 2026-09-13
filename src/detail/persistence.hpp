// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Internal persistence entry points.  Not installed.

#ifndef COHERENCE_SRC_DETAIL_PERSISTENCE_HPP
#define COHERENCE_SRC_DETAIL_PERSISTENCE_HPP

#include <filesystem>

#include "coherence/error.hpp"
#include "coherence/persistence.hpp"
#include "detail/state.hpp"

namespace sol::coherence::detail {

/// Writes durable structural state through a temporary file, then replaces
/// the destination atomically.
Result<PersistenceReport> save_state(const State& state, const std::filesystem::path& path,
                                     const PersistenceOptions& options);

/// Loads durable state and applies conservative recovery.
///
/// The file is fully parsed and validated into staging storage first; \p state
/// is modified only after the whole file has been accepted.  A failed load
/// therefore cannot partially apply.
Result<PersistenceReport> load_state(State& state, const std::filesystem::path& path,
                                     const PersistenceOptions& options);

}  // namespace sol::coherence::detail

#endif  // COHERENCE_SRC_DETAIL_PERSISTENCE_HPP
