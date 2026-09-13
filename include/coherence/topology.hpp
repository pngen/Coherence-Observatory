// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Topology generation binding.
//
// Locality and topology-edge attribution are only meaningful relative to the
// topology generation under which the evidence was produced.  Any topology
// mutation bumps the generation; attributions computed against an older
// generation are reported as STALE_EVIDENCE rather than silently reinterpreted.

#ifndef COHERENCE_TOPOLOGY_HPP
#define COHERENCE_TOPOLOGY_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "coherence/ids.hpp"
#include "coherence/locality.hpp"
#include "coherence/provenance.hpp"
#include "coherence/resource.hpp"
#include "coherence/time.hpp"

namespace sol::coherence {

/// A directed relationship between two resources.
struct COHERENCE_API TopologyLink {
  ResourceRef from;
  ResourceRef to;
  Locality locality = Locality::Unknown;
  /// Measured or vendor-reported latency; negative means unknown.
  std::int64_t latency_ns = -1;
  /// Vendor-reported bandwidth; 0 means unknown.
  std::uint64_t bandwidth_bytes_per_second = 0;
  /// Where the link description came from.
  Provenance provenance = Provenance::Unknown;
};

/// The complete topology as of one generation.
struct COHERENCE_API TopologyRecord {
  TopologyGeneration generation;
  std::vector<TopologyLink> links;
  Nanos created_at_ns = 0;
  bool loaded_from_state = false;

  /// Looks up the declared locality from p from to p to.
  ///
  /// Returns Locality::Unknown when no link is registered; the runtime never
  /// invents a relationship that the topology does not state.
  Locality locality_between(const ResourceRef& from, const ResourceRef& to) const;
};

}  // namespace sol::coherence

#endif  // COHERENCE_TOPOLOGY_HPP
