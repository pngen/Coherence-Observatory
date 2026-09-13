// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/region.hpp"

#include <array>
#include <cstring>

#include "text_util.hpp"

namespace sol::coherence {
namespace {

constexpr std::array<std::string_view, 6> kSharingScopeNames = {
    "UNKNOWN", "PRIVATE", "PROCESS_SHARED", "DEVICE_SHARED", "CROSS_NODE", "POOLED"};

/// splitmix64 finalizer: mixes a 64-bit value into a well-distributed one.
std::uint64_t mix64(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ull;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

}  // namespace

std::string_view to_string(SharingScope scope) noexcept {
  const auto index = static_cast<std::size_t>(scope);
  return index < kSharingScopeNames.size() ? kSharingScopeNames[index]
                                           : std::string_view("UNKNOWN");
}

Result<SharingScope> parse_sharing_scope(std::string_view text) {
  for (std::size_t i = 0; i < kSharingScopeNames.size(); ++i) {
    if (detail::iequals(kSharingScopeNames[i], text)) {
      return Result<SharingScope>(static_cast<SharingScope>(i));
    }
  }
  return Result<SharingScope>(
      Error(ErrorCode::InvalidArgument, "unknown sharing scope", std::string(text)));
}

std::uint64_t make_opaque_handle(const void* address, std::uint64_t owner_salt) noexcept {
  const std::uint64_t raw = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(address));
  return mix64(raw ^ mix64(owner_salt ^ 0xA5A5A5A5A5A5A5A5ull));
}

std::uint64_t region_identity_fingerprint(const RegionRecord& record) noexcept {
  std::uint64_t h = detail::fnv1a_init();
  h = detail::fnv1a_str(h, record.id.view());
  h = detail::fnv1a_u64(h, record.generation.value());
  h = detail::fnv1a_u64(h, record.opaque_handle);
  h = detail::fnv1a_str(h, record.owner);
  h = detail::fnv1a_str(h, record.memory_domain.view());
  h = detail::fnv1a_u64(h, record.memory_domain_generation.value());
  h = detail::fnv1a_str(h, record.coherence_domain.view());
  h = detail::fnv1a_u64(h, static_cast<std::uint64_t>(record.sharing_scope));
  h = detail::fnv1a_u64(h, record.size_bytes);
  h = detail::fnv1a_u64(h, record.page_size_bytes);
  h = detail::fnv1a_u64(h, record.allocation_generation);
  h = detail::fnv1a_u64(h, record.mapping_generation);
  h = detail::fnv1a_u64(h, record.retired ? 1u : 0u);
  return h;
}

}  // namespace sol::coherence
