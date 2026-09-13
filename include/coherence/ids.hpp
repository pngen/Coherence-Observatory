// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Strong identities.
//
// Only identities that guard a real stale-state, authority, or attribution
// boundary are modelled here.  Each generationbelow corresponds to something
// that can genuinely become stale, be superseded, or change meaning:
//
//   DeviceGeneration            hardware re-enumeration / driver reset
//   MemoryDomainGeneration      memory domain redefinition
//   MemoryRegionGeneration      re-allocation of the same region identity
//   CoherenceDomainGeneration   coherence domain redefinition
//   TopologyGeneration          any topology mutation
//   EvidenceGeneration          per-publisher evidence incarnation
//   CounterGeneration           counter continuity epoch
//   SamplingEpoch               collector sampling epoch
//   ObservationEpoch            coordinatoor-visible observation epoch
//   CoordinatorEpoch            coordinator process incarnation
//   SnapshotGeneration          monotonic snapshot identity
//
// Names are bounded, validated ASCII tokens; they are the only externally
// visible identity form.  Raw process addresses are never an identity here.

#ifndef COHERENCE_IDS_HPP
#define COHERENCE_IDS_HPP

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "coherence/error.hpp"
#include "coherence/limits.hpp"

namespace sol::coherence {

/// Validates an identity token: 1..Limits::kMaxNameLength characters from
/// [A-Za-z0-9._:@#-].  Rejects control characters, whitespace, path
/// separators and non-ASCII bytes so that identities stay canonical.
COHERENCE_API Status validate_identity_token(std::string_view token);

namespace detail {

/// Bounded, validated, comparable name identity.
template <class Tag>
class NameId {
 public:
  NameId() = default;
  explicit NameId(std::string value) : value_(std::move(value)) {}

  static Result<NameId> parse(std::string_view token) {
    const Status s = validate_identity_token(token);
    if (!s.ok()) {
      return Result<NameId>(s.error());
    }
    return Result<NameId>(NameId(std::string(token)));
  }

  bool empty() const noexcept { return value_.empty(); }
  const std::string& str() const noexcept { return value_; }
  std::string_view view() const noexcept { return value_; }

  friend bool operator==(const NameId&, const NameId&) = default;
  friend auto operator<=>(const NameId&, const NameId&) = default;

 private:
  std::string value_;
};

/// Monotonic counter identity: has a successor, used for epochs/generations.
template <class Tag>
class CounterId {
 public:
  using value_type = std::uint64_t;

  constexpr CounterId() = default;
  constexpr explicit CounterId(std::uint64_t value) : value_(value) {}

  static constexpr Result<CounterId> from(std::uint64_t value) {
    return Result<CounterId>(CounterId(value));
  }

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool is_zero() const noexcept { return value_ == 0; }
  constexpr CounterId next() const noexcept { return CounterId(value_ + 1); }

  friend constexpr bool operator==(const CounterId&, const CounterId&) = default;
  friend constexpr auto operator<=>(const CounterId&, const CounterId&) = default;

 private:
  std::uint64_t value_ = 0;
};

/// Opaque 64-bit identity: equality only, no meaningful ordering.
template <class Tag>
class OpaqueId {
 public:
  using value_type = std::uint64_t;

  constexpr OpaqueId() = default;
  constexpr explicit OpaqueId(std::uint64_t value) : value_(value) {}

  constexpr std::uint64_t value() const noexcept { return value_; }
  constexpr bool is_zero() const noexcept { return value_ == 0; }

  friend constexpr bool operator==(const OpaqueId&, const OpaqueId&) = default;

 private:
  std::uint64_t value_ = 0;
};

// Tag types -- one per distinct identity.
struct ObserverTag {};
struct PublisherTag {};
struct PublisherBootTag {};
struct NodeTag {};
struct ProcessorTag {};
struct AcceleratorTag {};
struct MemoryDomainTag {};
struct CoherenceDomainTag {};
struct MemoryRegionTag {};
struct WorkloadTag {};
struct ProcessTag {};
struct CounterTag {};
struct ResourceTag {};

struct DeviceGenerationTag {};
struct MemoryDomainGenerationTag {};
struct MemoryRegionGenerationTag {};
struct CoherenceDomainGenerationTag {};
struct TopologyGenerationTag {};
struct EvidenceGenerationTag {};
struct CounterGenerationTag {};
struct SamplingEpochTag {};
struct ObservationEpochTag {};
struct CoordinatorEpochTag {};
struct SnapshotGenerationTag {};

struct CoherenceEventTag {};
struct EventSequenceTag {};
struct AttributionTag {};
struct FindingTag {};

}  // namespace detail

// ---- Name identities -------------------------------------------------
using ObserverId = detail::NameId<detail::ObserverTag>;
using PublisherId = detail::NameId<detail::PublisherTag>;
using NodeId = detail::NameId<detail::NodeTag>;
using ProcessorId = detail::NameId<detail::ProcessorTag>;
using AcceleratorId = detail::NameId<detail::AcceleratorTag>;
using MemoryDomainId = detail::NameId<detail::MemoryDomainTag>;
using CoherenceDomainId = detail::NameId<detail::CoherenceDomainTag>;
using MemoryRegionId = detail::NameId<detail::MemoryRegionTag>;
using WorkloadId = detail::NameId<detail::WorkloadTag>;
using ProcessId = detail::NameId<detail::ProcessTag>;
using CounterId = detail::NameId<detail::CounterTag>;
using ResourceId = detail::NameId<detail::ResourceTag>;

// ---- Generation identities -------------------------------------------
using DeviceGeneration = detail::CounterId<detail::DeviceGenerationTag>;
using MemoryDomainGeneration = detail::CounterId<detail::MemoryDomainGenerationTag>;
using MemoryRegionGeneration = detail::CounterId<detail::MemoryRegionGenerationTag>;
using CoherenceDomainGeneration =
    detail::CounterId<detail::CoherenceDomainGenerationTag>;
using TopologyGeneration = detail::CounterId<detail::TopologyGenerationTag>;
using EvidenceGeneration = detail::CounterId<detail::EvidenceGenerationTag>;
using CounterGeneration = detail::CounterId<detail::CounterGenerationTag>;
using SamplingEpoch = detail::CounterId<detail::SamplingEpochTag>;
using ObservationEpoch = detail::CounterId<detail::ObservationEpochTag>;
using CoordinatorEpoch = detail::CounterId<detail::CoordinatorEpochTag>;
using SnapshotGeneration = detail::CounterId<detail::SnapshotGenerationTag>;

// ---- Event identities -------------------------------------------------
using CoherenceEventId = detail::OpaqueId<detail::CoherenceEventTag>;
using EventSequence = detail::CounterId<detail::EventSequenceTag>;
using AttributionId = detail::CounterId<detail::AttributionTag>;
using FindingId = detail::CounterId<detail::FindingTag>;

/// Incarnation identity of a publisher process boot.  A replacement publisher
/// process must present a fresh boot identity; a fenced boot identity is never
/// re-admitted.
using PublisherBootId = detail::OpaqueId<detail::PublisherBootTag>;

}  // namespace sol::coherence

namespace std {

template <class Tag>
struct hash<sol::coherence::detail::NameId<Tag>> {
  std::size_t operator()(const sol::coherence::detail::NameId<Tag>& id) const noexcept {
    return std::hash<std::string_view>{}(id.view());
  }
};

template <class Tag>
struct hash<sol::coherence::detail::CounterId<Tag>> {
  std::size_t operator()(const sol::coherence::detail::CounterId<Tag>& id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value());
  }
};

template <class Tag>
struct hash<sol::coherence::detail::OpaqueId<Tag>> {
  std::size_t operator()(const sol::coherence::detail::OpaqueId<Tag>& id) const noexcept {
    return std::hash<std::uint64_t>{}(id.value());
  }
};

}  // namespace std

#endif  // COHERENCE_IDS_HPP
