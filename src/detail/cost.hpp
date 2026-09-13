// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Internal cost estimation.  Not installed.

#ifndef COHERENCE_SRC_DETAIL_COST_HPP
#define COHERENCE_SRC_DETAIL_COST_HPP

#include <cstdint>

#include "coherence/cost.hpp"
#include "coherence/locality.hpp"
#include "coherence/precision.hpp"
#include "coherence/provenance.hpp"

namespace sol::coherence::detail {

/// Exact observed quantities a cost figure is computed from.
///
/// The counts and byte totals are facts taken from evidence; only the latency
/// figure is ever modelled.
struct CostInputs {
  std::uint64_t bytes = 0;
  std::uint64_t remote_reads = 0;
  std::uint64_t remote_writes = 0;
  std::uint64_t invalidations = 0;
  std::uint64_t ownership_transfers = 0;
  std::uint64_t retries = 0;
  std::uint64_t conflicts = 0;
  /// Measured duration supplied by the source; negative means unmeasured.
  std::int64_t measured_duration_ns = -1;
  Locality locality = Locality::Unknown;
  bool locality_known = false;
  Precision precision = Precision::Unknown;
};

/// Estimates cost under \p model.
///
/// The returned kind is MEASURED when a real measurement was supplied,
/// COUNTER_DERIVED when the latency arises from exact counter deltas and no
/// model is defined, MODEL_ESTIMATED when a defined model was applied, and
/// UNKNOWN when neither a measurement nor a model is available.  Every
/// contribution is decomposed into named terms.
CostEstimate estimate_cost(const CostModel& model, const CostInputs& inputs);

/// Model multiplier for an observed locality class.
double locality_multiplier(const CostModel& model, Locality locality) noexcept;

}  // namespace sol::coherence::detail

#endif  // COHERENCE_SRC_DETAIL_COST_HPP
