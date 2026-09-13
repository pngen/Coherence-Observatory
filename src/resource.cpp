// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/resource.hpp"

#include <array>
#include <iterator>

#include "text_util.hpp"

namespace sol::coherence {
namespace {

constexpr std::array<std::string_view, 7> kMemoryDomainKindNames = {
    "UNKNOWN",     "HOST_DRAM",       "DEVICE_MEMORY", "HIGH_BANDWIDTH_MEMORY",
    "CXL_ATTACHED", "POOLED_MEMORY", "REMOTE_NODE_MEMORY"};

}  // namespace

std::string_view to_string(MemoryDomainKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  return index < kMemoryDomainKindNames.size() ? kMemoryDomainKindNames[index]
                                               : std::string_view("UNKNOWN");
}

Result<MemoryDomainKind> parse_memory_domain_kind(std::string_view text) {
  for (std::size_t i = 0; i < kMemoryDomainKindNames.size(); ++i) {
    if (detail::iequals(kMemoryDomainKindNames[i], text)) {
      return Result<MemoryDomainKind>(static_cast<MemoryDomainKind>(i));
    }
  }
  return fail_as<MemoryDomainKind>(ErrorCode::InvalidArgument, "unknown memory domain kind",
                                   std::string(text));
}

}  // namespace sol::coherence
