// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Precision classes.
//
// Every observation states how precisely it knows what it claims.  Attribution
// and aggregation may only weaken precision, never strengthen it.

#ifndef COHERENCE_PRECISION_HPP
#define COHERENCE_PRECISION_HPP

#include <cstdint>
#include <string_view>

#include "coherence/error.hpp"
#include "coherence/export.hpp"

namespace sol::coherence {

/// How precisely the evidence resolves the reported fact.
///
/// Ordering is meaningful: a higher rank is strictly stronger evidence.
///   ExactEvent          one concrete event, individually identified
///   ExactCounterDelta   one exact delta of a counter that is genuinely scoped
///                       to the attributed subject
///   SampledEvent        a sampled (statistically extrapolated) event
///   AggregatedCounter   a counter covering a coarser subject than claimed
///   Derived             computed from other evidence by a stated rule
///   Inferred            suggested by indirect evidence, not proven
///   Unknown             precision could not be established
enum class Precision : std::uint8_t {
  Unknown = 0,
  Inferred = 1,
  Derived = 2,
  AggregatedCounter = 3,
  SampledEvent = 4,
  ExactCounterDelta = 5,
  ExactEvent = 6,
};

COHERENCE_API std::string_view to_string(Precision precision) noexcept;
COHERENCE_API Result<Precision> parse_precision(std::string_view text);

/// Numeric rank; larger is stronger.
COHERENCE_API std::uint8_t precision_rank(Precision precision) noexcept;

/// The weaker of two precisions.
COHERENCE_API Precision weakest(Precision a, Precision b) noexcept;

/// Weakest precision across a range.
COHERENCE_API Precision weakest_of(const Precision* first, const Precision* last) noexcept;

/// True when p claimed is no stronger than p supported.
COHERENCE_API bool precision_within(Precision claimed, Precision supported) noexcept;

/// Conservative precision ceiling implied by an evidence granularity.  A
/// device-wide counter can never support exact per-region claims.
COHERENCE_API Precision precision_ceiling_for_granularity(
    std::uint8_t granularity_rank_value) noexcept;

}  // namespace sol::coherence

#endif  // COHERENCE_PRECISION_HPP
