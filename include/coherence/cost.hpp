// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Coherence cost model.
//
// Cost estimation is always explicit and versioned, and the runtime always
// states whether a number was measured, derived from counters, or produced by
// a model.  An estimate is never presented as a measured quantity.

#ifndef COHERENCE_COST_HPP
#define COHERENCE_COST_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "coherence/error.hpp"
#include "coherence/ids.hpp"
#include "coherence/precision.hpp"
#include "coherence/taxonomy.hpp"

namespace sol::coherence {

/// How a cost figure was obtained.
enum class CostKind : std::uint8_t {
  /// Read from a real measurement supplied by the source.
  Measured = 0,
  /// Computed from exact counter deltas.
  CounterDerived = 1,
  /// Produced by applying an explicit cost model to observed counts/bytes.
  ModelEstimated = 2,
  /// Not estimable from the available evidence.
  Unknown = 3,
};

COHERENCE_API std::string_view to_string(CostKind kind) noexcept;

/// One named contribution to a cost figure.
struct COHERENCE_API CostTerm {
  /// Stable term key, e.g. "remote_read.latency".
  std::string name;
  CostDimension dimension = CostDimension::LatencyNanos;
  double value = 0.0;
  /// How this term was produced.
  CostKind kind = CostKind::Unknown;
  /// Coefficient applied, for model-estimated terms.
  double coefficient = 0.0;
  /// Observed quantity the coefficient was applied to.
  double quantity = 0.0;
  std::string unit;
};

/// Explicit, versioned coherence cost model.
///
/// Coefficients are supplied by the operator or by a measured calibration.
/// Nothing here is a hardware claim: the model is data, and the runtime
/// reports which model produced every estimated number.
struct COHERENCE_API CostModel {
  /// Stable model identity, e.g. "generic-numa-latency".
  std::string id = "unset";
  /// Model version; a change of coefficients must bump this.
  std::uint32_t version = 0;
  bool defined = false;

  /// Estimated nanoseconds per remote read.
  double remote_read_latency_ns = 0.0;
  /// Estimated nanoseconds per remote write.
  double remote_write_latency_ns = 0.0;
  /// Estimated nanoseconds per invalidation delivered.
  double invalidation_latency_ns = 0.0;
  /// Estimated nanoseconds per ownership transfer.
  double ownership_transfer_latency_ns = 0.0;
  /// Estimated nanoseconds per retry.
  double retry_latency_ns = 0.0;
  /// Estimated nanoseconds of stall per coherence conflict.
  double conflict_stall_ns = 0.0;
  /// Effective bandwidth in bytes per nanosecond used for transfer time.
  double bandwidth_bytes_per_ns = 0.0;
  /// Locality-class multipliers applied to the per-event latencies.
  double cxl_multiplier = 1.0;
  double pooled_multiplier = 1.0;
  double remote_node_multiplier = 1.0;

  /// Validates ranges and internal consistency.
  Status validate() const;
};

/// The generic NUMA-style default model, version 1.  Documented, not measured.
COHERENCE_API CostModel default_cost_model();

/// A cost figure with its full decomposition.
struct COHERENCE_API CostEstimate {
  CostKind kind = CostKind::Unknown;
  /// Total estimated or measured latency in nanoseconds.
  double latency_ns = 0.0;
  /// Bytes moved.
  std::uint64_t bytes = 0;
  /// Estimated bandwidth consumption in bytes per nanosecond.
  double bandwidth_bytes_per_ns = 0.0;
  std::uint64_t remote_accesses = 0;
  std::uint64_t ownership_transfers = 0;
  std::uint64_t invalidations = 0;
  std::uint64_t retries = 0;
  std::uint64_t stall_cycles = 0;
  /// Decomposition; always populated for ModelEstimated figures.
  std::vector<CostTerm> terms;
  /// Model that produced the estimate, when kind == ModelEstimated.
  std::string model_id;
  std::uint32_t model_version = 0;
  /// Precision of the evidence the cost rests on.
  Precision precision = Precision::Unknown;
  /// True when a term saturated and the total is a lower bound.
  bool lower_bound = false;
};

}  // namespace sol::coherence

#endif  // COHERENCE_COST_HPP
