// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Framed transport helpers shared by the coordinator and its clients.
// Not installed.

#ifndef COHERENCE_SRC_DETAIL_TRANSPORT_HPP
#define COHERENCE_SRC_DETAIL_TRANSPORT_HPP

#include <cstdint>
#include <vector>

#include "coherence/error.hpp"
#include "coherence/protocol.hpp"
#include "detail/platform.hpp"

namespace sol::coherence::detail {

/// Sends one complete frame, or fails without partial delivery semantics.
Status send_frame(const Socket& socket, const Frame& frame);

/// Reads one complete frame into \p buffer, which retains any partial bytes
/// already received.  Returns Truncated when the peer closed mid-frame.
Result<Frame> receive_frame(const Socket& socket, std::vector<std::uint8_t>& buffer);

}  // namespace sol::coherence::detail

#endif  // COHERENCE_SRC_DETAIL_TRANSPORT_HPP
