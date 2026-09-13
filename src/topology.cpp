// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/topology.hpp"

namespace sol::coherence {

Locality TopologyRecord::locality_between(const ResourceRef& from, const ResourceRef& to) const {
  if (from.empty() || to.empty()) {
    return Locality::Unknown;
  }
  if (from.kind == to.kind && from.id == to.id) {
    // A resource is local to itself only when identity and generation agree.
    return from.generation == to.generation ? Locality::LocalProcessor : Locality::Unknown;
  }
  for (const TopologyLink& link : links) {
    if (link.from.kind == from.kind && link.from.id == from.id &&
        link.to.kind == to.kind && link.to.id == to.id) {
      return link.locality;
    }
  }
  return Locality::Unknown;
}

}  // namespace sol::coherence
