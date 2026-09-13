// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Framed, versioned, integrity-checked wire protocol.
//
// Layout (all fields little-endian):
//
//   FrameHeader  (32 bytes)
//     char      magic[8]      "COBSFRM1"
//     uint16    version
//     uint16    type
//     uint32    flags
//     uint64    frame_sequence
//     uint32    payload_length
//     uint32    header_crc      CRC-32 over the header with this field zeroed
//   payload      (payload_length bytes)
//   uint32       payload_crc    CRC-32 over the payload bytes
//
// Every field is validated before the payload is trusted.

#ifndef COHERENCE_PROTOCOL_HPP
#define COHERENCE_PROTOCOL_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "coherence/error.hpp"
#include "coherence/export.hpp"
#include "coherence/ids.hpp"
#include "coherence/limits.hpp"

namespace sol::coherence {

/// Wire protocol version implemented by this build.
inline constexpr std::uint16_t kWireProtocolVersion = 1;

/// Frame magic: "COBSFRM1".
inline constexpr char kFrameMagic[8] = {'C', 'O', 'B', 'S', 'F', 'R', 'M', '1'};

/// Message types.
enum class MessageType : std::uint16_t {
  Hello = 1,
  HelloAck = 2,
  RegisterPublisher = 3,
  RegisterResource = 4,
  RegisterRegion = 5,
  RegisterCoherenceDomain = 6,
  SetTopologyLink = 7,
  PublishEvent = 8,
  PublishBatch = 9,
  PublishCounter = 10,
  QuerySnapshot = 11,
  QueryFindings = 12,
  QueryAttribution = 13,
  Fence = 14,
  Heartbeat = 15,
  SaveState = 16,
  BumpTopology = 17,
  RetireRegion = 18,
  Ack = 100,
  Error = 101,
  SnapshotResponse = 102,
  FindingsResponse = 103,
  AttributionResponse = 104,
};

COHERENCE_API std::string_view to_string(MessageType type) noexcept;
COHERENCE_API Result<MessageType> parse_message_type(std::uint16_t raw) noexcept;

/// Client role presented during HELLO.  Authority is role-based: only Admin
/// connections may mutate structural state.
enum class ClientRole : std::uint8_t {
  /// May register itself and publish observations for itself only.
  Publisher = 0,
  /// Read-only queries.
  Client = 1,
  /// Structural mutation and administrative operations.
  Admin = 2,
};

COHERENCE_API std::string_view to_string(ClientRole role) noexcept;

/// Frame header, exactly as it appears on the wire.
struct COHERENCE_API FrameHeader {
  char magic[8] = {'C', 'O', 'B', 'S', 'F', 'R', 'M', '1'};
  std::uint16_t version = kWireProtocolVersion;
  std::uint16_t type = 0;
  std::uint32_t flags = 0;
  std::uint64_t frame_sequence = 0;
  std::uint32_t payload_length = 0;
  std::uint32_t header_crc = 0;

  /// Serializes to exactly Limits::kFrameHeaderSize bytes.
  void encode(std::uint8_t* out) const noexcept;
  /// Parses and validates magic/version/flags/crc.
  static Status decode(const std::uint8_t* in, std::size_t size, FrameHeader* out);
};

/// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320).
COHERENCE_API std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept;
COHERENCE_API std::uint32_t crc32_extend(std::uint32_t seed, const std::uint8_t* data,
                                         std::size_t size) noexcept;

/// Bounded little-endian writer with checked lengths.
class COHERENCE_API PayloadWriter {
 public:
  void u8(std::uint8_t value);
  /// Convenience overload so boolean fields serialize without implicit
  /// narrowing conversions.
  void u8(bool value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void f64(double value);
  void bytes(const std::uint8_t* data, std::size_t size);
  /// Writes a length-prefixed UTF-8 string; fails past the bound.
  Status text(std::string_view value, std::size_t max_length);

  const std::vector<std::uint8_t>& data() const noexcept { return data_; }
  std::size_t size() const noexcept { return data_.size(); }
  void clear() noexcept { data_.clear(); }

 private:
  std::vector<std::uint8_t> data_;
};

/// Bounded little-endian reader.  Every read is bounds-checked; overruns and
/// trailing bytes are reported instead of ignored.
class COHERENCE_API PayloadReader {
 public:
  PayloadReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

  Status u8(std::uint8_t* out);
  Status u16(std::uint16_t* out);
  Status u32(std::uint32_t* out);
  Status u64(std::uint64_t* out);
  Status i64(std::int64_t* out);
  Status f64(double* out);
  Status bytes(std::uint8_t* out, std::size_t count);
  Status text(std::string* out, std::size_t max_length);

  std::size_t remaining() const noexcept { return size_ - position_; }
  std::size_t position() const noexcept { return position_; }
  bool exhausted() const noexcept { return position_ == size_; }

  /// Fails unless the reader is fully consumed.
  Status require_exhausted() const;

 private:
  const std::uint8_t* data_ = nullptr;
  std::size_t size_ = 0;
  std::size_t position_ = 0;
};

/// A complete frame ready for transport.
struct COHERENCE_API Frame {
  MessageType type = MessageType::Hello;
  std::uint32_t flags = 0;
  std::uint64_t frame_sequence = 0;
  std::vector<std::uint8_t> payload;

  /// Serializes header + payload + trailer.
  std::vector<std::uint8_t> encode() const;

  /// Parses one complete frame from p data.
  ///
  /// Fails on short input, bad magic, unsupported version, unknown type,
  /// invalid flags, oversized payload, truncation, bad integrity or trailing
  /// bytes.
  static Result<Frame> decode(const std::uint8_t* data, std::size_t size);
};

/// Frame flags.
namespace frame_flags {
inline constexpr std::uint32_t kNone = 0;
/// The sender requires an acknowledgement.
inline constexpr std::uint32_t kAckRequired = 1u << 0;
/// The sender is responding to a previous frame.
inline constexpr std::uint32_t kResponse = 1u << 1;
inline constexpr std::uint32_t kKnownMask = kAckRequired | kResponse;
}  // namespace frame_flags

/// Status codes carried in Ack/Error payloads.
enum class AckStatus : std::uint16_t {
  Accepted = 0,
  PartiallyAccepted = 1,
  Rejected = 2,
  Unauthorized = 3,
  StaleEpoch = 4,
  StaleBoot = 5,
  StaleGeneration = 6,
  StaleSequence = 7,
  Malformed = 8,
  Capacity = 9,
  Unsupported = 10,
  InternalError = 11,
};

COHERENCE_API std::string_view to_string(AckStatus status) noexcept;
COHERENCE_API AckStatus ack_status_for(ErrorCode code) noexcept;

}  // namespace sol::coherence

#endif  // COHERENCE_PROTOCOL_HPP
