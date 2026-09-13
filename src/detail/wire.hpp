// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Bounded wire codecs for protocol payloads.  Not installed.
//
// Every decoder validates bounds and enum ranges before the value is used, and
// every decoder reports trailing bytes instead of ignoring them.

#ifndef COHERENCE_SRC_DETAIL_WIRE_HPP
#define COHERENCE_SRC_DETAIL_WIRE_HPP

#include "coherence/error.hpp"
#include "coherence/observation.hpp"
#include "coherence/observatory.hpp"
#include "coherence/protocol.hpp"
#include "coherence/region.hpp"
#include "coherence/resource.hpp"
#include "coherence/snapshot.hpp"
#include "coherence/topology.hpp"

namespace sol::coherence::detail {

// ---- Observations ------------------------------------------------------
Status write_observation(PayloadWriter& writer, const Observation& observation);
Result<Observation> read_observation(PayloadReader& reader);

Status write_counter_publication(PayloadWriter& writer, const CounterPublication& publication);
Result<CounterPublication> read_counter_publication(PayloadReader& reader);

Status write_publisher_registration(PayloadWriter& writer,
                                    const PublisherRegistration& registration);
Result<PublisherRegistration> read_publisher_registration(PayloadReader& reader);

// ---- Structural records ------------------------------------------------
Status write_node(PayloadWriter& writer, const NodeRecord& record);
Result<NodeRecord> read_node(PayloadReader& reader);

Status write_processor(PayloadWriter& writer, const ProcessorRecord& record);
Result<ProcessorRecord> read_processor(PayloadReader& reader);

Status write_accelerator(PayloadWriter& writer, const AcceleratorRecord& record);
Result<AcceleratorRecord> read_accelerator(PayloadReader& reader);

Status write_memory_domain(PayloadWriter& writer, const MemoryDomainRecord& record);
Result<MemoryDomainRecord> read_memory_domain(PayloadReader& reader);

Status write_coherence_domain(PayloadWriter& writer, const CoherenceDomainRecord& record);
Result<CoherenceDomainRecord> read_coherence_domain(PayloadReader& reader);

Status write_region(PayloadWriter& writer, const RegionRecord& record);
Result<RegionRecord> read_region(PayloadReader& reader);

Status write_topology_link(PayloadWriter& writer, const TopologyLink& link);
Result<TopologyLink> read_topology_link(PayloadReader& reader);

Status write_capability(PayloadWriter& writer, const Capability& capability);
Result<Capability> read_capability(PayloadReader& reader);

// ---- Results -----------------------------------------------------------
Status write_finding(PayloadWriter& writer, const Finding& finding);
Result<Finding> read_finding(PayloadReader& reader);

Status write_attribution(PayloadWriter& writer, const AttributionResult& attribution);
Result<AttributionResult> read_attribution(PayloadReader& reader);

Status write_snapshot(PayloadWriter& writer, const Snapshot& snapshot);
Result<SnapshotPtr> read_snapshot(PayloadReader& reader);

/// Acknowledgement / error payload.
struct AckPayload {
  AckStatus status = AckStatus::Accepted;
  CoordinatorEpoch coordinator_epoch;
  ObservationEpoch observation_epoch;
  std::string detail;
  std::uint64_t accepted = 0;
  std::uint64_t rejected = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t late = 0;
  std::uint64_t missing = 0;
  std::uint64_t produced_delta = 0;
  std::uint64_t delta = 0;
  std::uint64_t discontinuity = 0;
};

Status write_ack(PayloadWriter& writer, const AckPayload& ack);
Result<AckPayload> read_ack(PayloadReader& reader);

}  // namespace sol::coherence::detail

#endif  // COHERENCE_SRC_DETAIL_WIRE_HPP
