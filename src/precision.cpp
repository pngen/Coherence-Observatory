// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/precision.hpp"

#include <array>
#include <iterator>

#include "text_util.hpp"

namespace sol::coherence {
namespace {

constexpr std::array<std::string_view, 7> kPrecisionNames = {
    "UNKNOWN", "INFERRED", "DERIVED", "AGGREGATED_COUNTER",
    "SAMPLED_EVENT", "EXACT_COUNTER_DELTA", "EXACT_EVENT"};

}  // namespace

std::string_view to_string(Precision precision) noexcept {
  const auto index = static_cast<std::size_t>(precision);
  return index < kPrecisionNames.size() ? kPrecisionNames[index] : std::string_view("UNKNOWN");
}

Result<Precision> parse_precision(std::string_view text) {
  for (std::size_t i = 0; i < kPrecisionNames.size(); ++i) {
    if (detail::iequals(kPrecisionNames[i], text)) {
      return Result<Precision>(static_cast<Precision>(i));
    }
  }
  return Result<Precision>(Error(ErrorCode::InvalidArgument, "unknown precision",
                                 std::string(text)));
}

std::uint8_t precision_rank(Precision precision) noexcept {
  const auto index = static_cast<std::size_t>(precision);
  return index < kPrecisionNames.size() ? static_cast<std::uint8_t>(index)
                                        : static_cast<std::uint8_t>(0);
}

Precision weakest(Precision a, Precision b) noexcept {
  return precision_rank(a) <= precision_rank(b) ? a : b;
}

Precision weakest_of(const Precision* first, const Precision* last) noexcept {
  Precision result = Precision::ExactEvent;
  bool any = false;
  for (const Precision* it = first; it != last; ++it) {
    result = any ? weakest(result, *it) : *it;
    any = true;
  }
  return any ? result : Precision::Unknown;
}

bool precision_within(Precision claimed, Precision supported) noexcept {
  return precision_rank(claimed) <= precision_rank(supported);
}

Precision precision_ceiling_for_granularity(std::uint8_t granularity_rank_value) noexcept {
  switch (granularity_rank_value) {
    case 6: return Precision::ExactEvent;          // cache line
    case 5: return Precision::SampledEvent;        // page
    case 4: return Precision::SampledEvent;        // region
    case 3: return Precision::AggregatedCounter;   // device
    case 2: return Precision::AggregatedCounter;   // domain
    case 1: return Precision::AggregatedCounter;   // aggregate
    default: return Precision::Unknown;
  }
}

}  // namespace sol::coherence
