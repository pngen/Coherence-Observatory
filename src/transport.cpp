// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "detail/transport.hpp"

#include <array>

namespace sol::coherence::detail {

Status send_frame(const Socket& socket, const Frame& frame) {
  const std::vector<std::uint8_t> encoded = frame.encode();
  return socket_send_all(socket, encoded.data(), encoded.size());
}

Result<Frame> receive_frame(const Socket& socket, std::vector<std::uint8_t>& buffer) {
  std::array<std::uint8_t, 16384> chunk{};
  while (true) {
    if (buffer.size() >= Limits::kFrameHeaderSize) {
      FrameHeader header;
      const Status decoded =
          FrameHeader::decode(buffer.data(), buffer.size(), &header);
      if (!decoded.ok()) {
        return Result<Frame>(decoded.error());
      }
      const std::size_t total =
          Limits::kFrameHeaderSize + header.payload_length + Limits::kFrameTrailerSize;
      if (buffer.size() >= total) {
        Result<Frame> frame = Frame::decode(buffer.data(), total);
        if (!frame.ok()) {
          return frame;
        }
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(total));
        return frame;
      }
    }
    const Result<std::size_t> received = socket_recv_some(socket, chunk.data(), chunk.size());
    if (!received.ok()) {
      return Result<Frame>(received.error());
    }
    if (received.value() == 0) {
      return fail_as<Frame>(ErrorCode::ConnectionClosed, "peer closed the connection");
    }
    buffer.insert(buffer.end(), chunk.begin(),
                  chunk.begin() + static_cast<std::ptrdiff_t>(received.value()));
    if (buffer.size() > Limits::kMaxFramePayload + Limits::kFrameHeaderSize +
                             Limits::kFrameTrailerSize) {
      return fail_as<Frame>(ErrorCode::TooLarge, "receive buffer exceeds the frame bound");
    }
  }
}

}  // namespace sol::coherence::detail
