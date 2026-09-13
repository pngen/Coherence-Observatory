// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/locality.hpp"

#include <array>
#include <iterator>

#include "text_util.hpp"

namespace sol::coherence {
namespace {

constexpr std::array<std::string_view, kLocalityCount> kLocalityNames = {
    "UNKNOWN",      "LOCAL_PROCESSOR",    "LOCAL_NUMA",        "REMOTE_NUMA",
    "LOCAL_ACCELERATOR", "PEER_ACCELERATOR", "SAME_ROOT_COMPLEX", "REMOTE_ACCELERATOR",
    "CXL_ATTACHED", "POOLED_MEMORY",      "REMOTE_NODE"};

static_assert(kLocalityNames.size() == kLocalityCount, "locality table must match enum");

}  // namespace

std::string_view to_string(Locality locality) noexcept {
  const auto index = static_cast<std::size_t>(locality);
  return index < kLocalityNames.size() ? kLocalityNames[index] : std::string_view("UNKNOWN");
}

Result<Locality> parse_locality(std::string_view text) {
  for (std::size_t i = 0; i < kLocalityNames.size(); ++i) {
    if (detail::iequals(kLocalityNames[i], text)) {
      return Result<Locality>(static_cast<Locality>(i));
    }
  }
  return Result<Locality>(Error(ErrorCode::InvalidArgument, "unknown locality", std::string(text)));
}

bool is_remote_locality(Locality locality) noexcept {
  switch (locality) {
    case Locality::RemoteNuma:
    case Locality::RemoteAccelerator:
    case Locality::RemoteNode:
    case Locality::CxlAttached:
    case Locality::PooledMemory:
      return true;
    default:
      return false;
  }
}

bool is_cxl_class_locality(Locality locality) noexcept {
  return locality == Locality::CxlAttached || locality == Locality::PooledMemory;
}

}  // namespace sol::coherence
