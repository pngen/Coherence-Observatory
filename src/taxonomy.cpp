// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/taxonomy.hpp"

#include <array>
#include <iterator>

#include "text_util.hpp"

namespace sol::coherence {
namespace {

constexpr std::array<std::string_view, kEventTypeCount> kEventNames = {
    "REMOTE_READ",
    "REMOTE_WRITE",
    "INVALIDATION",
    "OWNERSHIP_TRANSFER",
    "OWNERSHIP_UPGRADE",
    "OWNERSHIP_DOWNGRADE",
    "SHARED_READ",
    "WRITE_EXCLUSIVE_TRANSITION",
    "CACHE_TO_CACHE_TRANSFER",
    "MEMORY_DOMAIN_TRANSFER",
    "SNOOP_REQUEST",
    "SNOOP_RESPONSE",
    "DIRECTORY_LOOKUP",
    "DIRECTORY_MISS",
    "COHERENCE_RETRY",
    "COHERENCE_CONFLICT",
    "WRITEBACK",
    "REMOTE_ATOMIC",
    "CONSISTENCY_BARRIER",
    "FLUSH",
    "FENCE",
    "REGION_INVALIDATION",
    "UNKNOWN_COHERENCE_EVENT",
};

constexpr std::array<std::string_view, 6> kDirectionNames = {
    "NONE", "READ", "WRITE", "READ_MODIFY_WRITE", "ATOMIC", "UNKNOWN"};

constexpr std::array<std::string_view, 10> kCoherenceStateNames = {
    "UNKNOWN", "INVALID", "SHARED", "EXCLUSIVE", "MODIFIED",
    "OWNED",   "FORWARD", "SHARED_CLEAN", "SHARED_DIRTY", "EXCLUSIVE_DIRTY"};

constexpr std::array<std::string_view, 7> kGranularityNames = {
    "UNKNOWN", "CACHE_LINE", "PAGE", "REGION", "DEVICE", "DOMAIN", "AGGREGATE"};

constexpr std::array<std::string_view, 6> kResourceKindNames = {
    "UNKNOWN", "NODE", "PROCESSOR", "ACCELERATOR", "MEMORY_DOMAIN", "COHERENCE_DOMAIN"};

constexpr std::array<std::string_view, 5> kCounterKindNames = {
    "ABSOLUTE", "DELTA", "RESETTABLE", "WRAPPING", "SAMPLED"};

constexpr std::array<std::string_view, 7> kCounterScopeNames = {
    "UNKNOWN", "PER_REGION", "PER_DEVICE", "PER_DOMAIN", "PER_LINK", "PER_PROCESS", "GLOBAL"};

constexpr std::array<std::string_view, 14> kCostDimensionNames = {
    "BYTES_TRANSFERRED", "LATENCY_NANOS", "BANDWIDTH_BYTES_PER_SECOND", "STALL_CYCLES",
    "RETRY_OVERHEAD", "INVALIDATED_BYTES", "WRITEBACK_BYTES", "REMOTE_MEMORY_ACCESSES",
    "OWNERSHIP_TRANSFERS", "CROSS_DOMAIN_TRAFFIC", "CROSS_NUMA_TRAFFIC",
    "CROSS_ACCELERATOR_TRAFFIC", "CXL_CLASS_TRAFFIC", "EXECUTION_TIME_IMPACT"};

template <class Enum, std::size_t N>
Result<Enum> parse_from_table(const std::array<std::string_view, N>& table,
                              std::string_view text, std::string_view what) {
  for (std::size_t i = 0; i < N; ++i) {
    if (detail::iequals(table[i], text)) {
      return Result<Enum>(static_cast<Enum>(i));
    }
  }
  return Result<Enum>(Error(ErrorCode::InvalidArgument,
                            std::string("unknown ") + std::string(what),
                            std::string(text)));
}

}  // namespace

std::string_view to_string(EventType type) noexcept {
  const auto index = static_cast<std::size_t>(type);
  if (index >= kEventNames.size()) {
    return kEventNames[static_cast<std::size_t>(EventType::UnknownCoherenceEvent)];
  }
  return kEventNames[index];
}

Result<EventType> parse_event_type(std::string_view text) {
  if (detail::iequals(text, "UNKNOWN")) {
    return Result<EventType>(EventType::UnknownCoherenceEvent);
  }
  return parse_from_table<EventType>(kEventNames, text, "event type");
}

bool is_ownership_event(EventType type) noexcept {
  switch (type) {
    case EventType::OwnershipTransfer:
    case EventType::OwnershipUpgrade:
    case EventType::OwnershipDowngrade:
    case EventType::WriteExclusiveTransition:
    case EventType::CacheToCacheTransfer:
    case EventType::MemoryDomainTransfer:
      return true;
    default:
      return false;
  }
}

bool is_traffic_event(EventType type) noexcept {
  switch (type) {
    case EventType::RemoteRead:
    case EventType::RemoteWrite:
    case EventType::RemoteAtomic:
    case EventType::CacheToCacheTransfer:
    case EventType::MemoryDomainTransfer:
    case EventType::Writeback:
    case EventType::Flush:
    case EventType::SharedRead:
      return true;
    default:
      return false;
  }
}

std::string_view to_string(AccessDirection direction) noexcept {
  const auto index = static_cast<std::size_t>(direction);
  return index < kDirectionNames.size() ? kDirectionNames[index] : std::string_view("UNKNOWN");
}

Result<AccessDirection> parse_access_direction(std::string_view text) {
  return parse_from_table<AccessDirection>(kDirectionNames, text, "access direction");
}

std::string_view to_string(CoherenceState state) noexcept {
  const auto index = static_cast<std::size_t>(state);
  return index < kCoherenceStateNames.size() ? kCoherenceStateNames[index]
                                             : std::string_view("UNKNOWN");
}

Result<CoherenceState> parse_coherence_state(std::string_view text) {
  return parse_from_table<CoherenceState>(kCoherenceStateNames, text, "coherence state");
}

std::string_view to_string(EvidenceGranularity granularity) noexcept {
  const auto index = static_cast<std::size_t>(granularity);
  return index < kGranularityNames.size() ? kGranularityNames[index]
                                          : std::string_view("UNKNOWN");
}

Result<EvidenceGranularity> parse_evidence_granularity(std::string_view text) {
  return parse_from_table<EvidenceGranularity>(kGranularityNames, text, "evidence granularity");
}

std::uint8_t granularity_rank(EvidenceGranularity granularity) noexcept {
  switch (granularity) {
    case EvidenceGranularity::CacheLine: return 6;
    case EvidenceGranularity::Page: return 5;
    case EvidenceGranularity::Region: return 4;
    case EvidenceGranularity::Device: return 3;
    case EvidenceGranularity::Domain: return 2;
    case EvidenceGranularity::Aggregate: return 1;
    case EvidenceGranularity::Unknown: return 0;
  }
  return 0;
}

std::string_view to_string(ResourceKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  return index < kResourceKindNames.size() ? kResourceKindNames[index]
                                           : std::string_view("UNKNOWN");
}

Result<ResourceKind> parse_resource_kind(std::string_view text) {
  return parse_from_table<ResourceKind>(kResourceKindNames, text, "resource kind");
}

std::string_view to_string(CounterKind kind) noexcept {
  const auto index = static_cast<std::size_t>(kind);
  return index < kCounterKindNames.size() ? kCounterKindNames[index]
                                          : std::string_view("ABSOLUTE");
}

Result<CounterKind> parse_counter_kind(std::string_view text) {
  return parse_from_table<CounterKind>(kCounterKindNames, text, "counter kind");
}

std::string_view to_string(CounterScope scope) noexcept {
  const auto index = static_cast<std::size_t>(scope);
  return index < kCounterScopeNames.size() ? kCounterScopeNames[index]
                                           : std::string_view("UNKNOWN");
}

Result<CounterScope> parse_counter_scope(std::string_view text) {
  return parse_from_table<CounterScope>(kCounterScopeNames, text, "counter scope");
}

std::string_view to_string(CostDimension dimension) noexcept {
  const auto index = static_cast<std::size_t>(dimension);
  return index < kCostDimensionNames.size() ? kCostDimensionNames[index]
                                            : std::string_view("UNKNOWN");
}

}  // namespace sol::coherence
