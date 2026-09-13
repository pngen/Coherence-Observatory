// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/protocol.hpp"

#include <array>
#include <cstring>

#include "text_util.hpp"

namespace sol::coherence {
namespace {

std::array<std::uint32_t, 256> make_crc_table() {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t i = 0; i < 256; ++i) {
    std::uint32_t value = i;
    for (int bit = 0; bit < 8; ++bit) {
      value = (value & 1u) != 0u ? (value >> 1) ^ 0xEDB88320u : (value >> 1);
    }
    table[i] = value;
  }
  return table;
}

const std::array<std::uint32_t, 256>& crc_table() {
  static const std::array<std::uint32_t, 256> table = make_crc_table();
  return table;
}

void put_u16(std::uint8_t* out, std::uint16_t value) noexcept {
  out[0] = static_cast<std::uint8_t>(value & 0xFFu);
  out[1] = static_cast<std::uint8_t>((value >> 8) & 0xFFu);
}

void put_u32(std::uint8_t* out, std::uint32_t value) noexcept {
  for (int i = 0; i < 4; ++i) {
    out[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

void put_u64(std::uint8_t* out, std::uint64_t value) noexcept {
  for (int i = 0; i < 8; ++i) {
    out[i] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu);
  }
}

std::uint16_t get_u16(const std::uint8_t* in) noexcept {
  return static_cast<std::uint16_t>(static_cast<std::uint16_t>(in[0]) |
                                    (static_cast<std::uint16_t>(in[1]) << 8));
}

std::uint32_t get_u32(const std::uint8_t* in) noexcept {
  std::uint32_t value = 0;
  for (int i = 0; i < 4; ++i) {
    value |= static_cast<std::uint32_t>(in[i]) << (8 * i);
  }
  return value;
}

std::uint64_t get_u64(const std::uint8_t* in) noexcept {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= static_cast<std::uint64_t>(in[i]) << (8 * i);
  }
  return value;
}

bool is_known_message_type(std::uint16_t raw) noexcept {
  switch (static_cast<MessageType>(raw)) {
    case MessageType::Hello:
    case MessageType::HelloAck:
    case MessageType::RegisterPublisher:
    case MessageType::RegisterResource:
    case MessageType::RegisterRegion:
    case MessageType::RegisterCoherenceDomain:
    case MessageType::SetTopologyLink:
    case MessageType::PublishEvent:
    case MessageType::PublishBatch:
    case MessageType::PublishCounter:
    case MessageType::QuerySnapshot:
    case MessageType::QueryFindings:
    case MessageType::QueryAttribution:
    case MessageType::Fence:
    case MessageType::Heartbeat:
    case MessageType::SaveState:
    case MessageType::BumpTopology:
    case MessageType::RetireRegion:
    case MessageType::Ack:
    case MessageType::Error:
    case MessageType::SnapshotResponse:
    case MessageType::FindingsResponse:
    case MessageType::AttributionResponse:
      return true;
  }
  return false;
}

}  // namespace

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) noexcept {
  return crc32_extend(0xFFFFFFFFu, data, size) ^ 0xFFFFFFFFu;
}

std::uint32_t crc32_extend(std::uint32_t seed, const std::uint8_t* data,
                           std::size_t size) noexcept {
  const std::array<std::uint32_t, 256>& table = crc_table();
  std::uint32_t value = seed;
  for (std::size_t i = 0; i < size; ++i) {
    value = table[(value ^ data[i]) & 0xFFu] ^ (value >> 8);
  }
  return value;
}

void FrameHeader::encode(std::uint8_t* out) const noexcept {
  std::memcpy(out, magic, 8);
  put_u16(out + 8, version);
  put_u16(out + 10, type);
  put_u32(out + 12, flags);
  put_u64(out + 16, frame_sequence);
  put_u32(out + 24, payload_length);
  put_u32(out + 28, 0);
  const std::uint32_t crc = crc32(out, Limits::kFrameHeaderSize);
  put_u32(out + 28, crc);
}

Status FrameHeader::decode(const std::uint8_t* in, std::size_t size, FrameHeader* out) {
  if (out == nullptr) {
    return fail(ErrorCode::InvalidArgument, "null frame header destination");
  }
  if (size < Limits::kFrameHeaderSize) {
    return fail(ErrorCode::Truncated, "frame header is short",
                detail::format_u64(size));
  }
  if (std::memcmp(in, kFrameMagic, 8) != 0) {
    return fail(ErrorCode::InvalidMagic, "frame magic mismatch");
  }
  FrameHeader header;
  std::memcpy(header.magic, in, 8);
  header.version = get_u16(in + 8);
  header.type = get_u16(in + 10);
  header.flags = get_u32(in + 12);
  header.frame_sequence = get_u64(in + 16);
  header.payload_length = get_u32(in + 24);
  header.header_crc = get_u32(in + 28);

  if (header.version != kWireProtocolVersion) {
    return fail(ErrorCode::UnsupportedVersion, "unsupported protocol version",
                detail::format_u64(header.version));
  }
  if (!is_known_message_type(header.type)) {
    return fail(ErrorCode::UnknownMessageType, "unknown message type",
                detail::format_u64(header.type));
  }
  if ((header.flags & ~frame_flags::kKnownMask) != 0) {
    return fail(ErrorCode::InvalidFlags, "unknown frame flag bits",
                detail::format_u64(header.flags));
  }
  if (header.payload_length > Limits::kMaxFramePayload) {
    return fail(ErrorCode::TooLarge, "frame payload exceeds the maximum size",
                detail::format_u64(header.payload_length));
  }
  std::array<std::uint8_t, Limits::kFrameHeaderSize> copy{};
  std::memcpy(copy.data(), in, Limits::kFrameHeaderSize);
  put_u32(copy.data() + 28, 0);
  if (crc32(copy.data(), copy.size()) != header.header_crc) {
    return fail(ErrorCode::IntegrityFailure, "frame header checksum mismatch");
  }
  *out = header;
  return Status();
}

std::vector<std::uint8_t> Frame::encode() const {
  std::vector<std::uint8_t> out;
  out.resize(Limits::kFrameHeaderSize + payload.size() + Limits::kFrameTrailerSize);
  FrameHeader header;
  header.type = static_cast<std::uint16_t>(type);
  header.flags = flags;
  header.frame_sequence = frame_sequence;
  header.payload_length = static_cast<std::uint32_t>(payload.size());
  header.encode(out.data());
  if (!payload.empty()) {
    std::memcpy(out.data() + Limits::kFrameHeaderSize, payload.data(), payload.size());
  }
  put_u32(out.data() + Limits::kFrameHeaderSize + payload.size(),
          crc32(payload.data(), payload.size()));
  return out;
}

Result<Frame> Frame::decode(const std::uint8_t* data, std::size_t size) {
  FrameHeader header;
  const Status header_status = FrameHeader::decode(data, size, &header);
  if (!header_status.ok()) {
    return Result<Frame>(header_status.error());
  }
  const std::size_t expected =
      Limits::kFrameHeaderSize + header.payload_length + Limits::kFrameTrailerSize;
  if (size < expected) {
    return fail_as<Frame>(ErrorCode::Truncated, "frame is truncated",
                          std::string("expected=") + detail::format_u64(expected) +
                              " actual=" + detail::format_u64(size));
  }
  if (size > expected) {
    return fail_as<Frame>(ErrorCode::TrailingBytes,
                          "bytes follow the declared frame length",
                          std::string("expected=") + detail::format_u64(expected) +
                              " actual=" + detail::format_u64(size));
  }
  const std::uint8_t* payload = data + Limits::kFrameHeaderSize;
  const std::uint32_t declared =
      get_u32(data + Limits::kFrameHeaderSize + header.payload_length);
  if (crc32(payload, header.payload_length) != declared) {
    return fail_as<Frame>(ErrorCode::IntegrityFailure, "frame payload checksum mismatch");
  }
  Frame frame;
  frame.type = static_cast<MessageType>(header.type);
  frame.flags = header.flags;
  frame.frame_sequence = header.frame_sequence;
  frame.payload.assign(payload, payload + header.payload_length);
  return Result<Frame>(std::move(frame));
}

std::string_view to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::Hello: return "HELLO";
    case MessageType::HelloAck: return "HELLO_ACK";
    case MessageType::RegisterPublisher: return "REGISTER_PUBLISHER";
    case MessageType::RegisterResource: return "REGISTER_RESOURCE";
    case MessageType::RegisterRegion: return "REGISTER_REGION";
    case MessageType::RegisterCoherenceDomain: return "REGISTER_COHERENCE_DOMAIN";
    case MessageType::SetTopologyLink: return "SET_TOPOLOGY_LINK";
    case MessageType::PublishEvent: return "PUBLISH_EVENT";
    case MessageType::PublishBatch: return "PUBLISH_BATCH";
    case MessageType::PublishCounter: return "PUBLISH_COUNTER";
    case MessageType::QuerySnapshot: return "QUERY_SNAPSHOT";
    case MessageType::QueryFindings: return "QUERY_FINDINGS";
    case MessageType::QueryAttribution: return "QUERY_ATTRIBUTION";
    case MessageType::Fence: return "FENCE";
    case MessageType::Heartbeat: return "HEARTBEAT";
    case MessageType::SaveState: return "SAVE_STATE";
    case MessageType::BumpTopology: return "BUMP_TOPOLOGY";
    case MessageType::RetireRegion: return "RETIRE_REGION";
    case MessageType::Ack: return "ACK";
    case MessageType::Error: return "ERROR";
    case MessageType::SnapshotResponse: return "SNAPSHOT_RESPONSE";
    case MessageType::FindingsResponse: return "FINDINGS_RESPONSE";
    case MessageType::AttributionResponse: return "ATTRIBUTION_RESPONSE";
  }
  return "UNKNOWN";
}

Result<MessageType> parse_message_type(std::uint16_t raw) noexcept {
  if (!is_known_message_type(raw)) {
    return fail_as<MessageType>(ErrorCode::UnknownMessageType, "unknown message type",
                                detail::format_u64(raw));
  }
  return Result<MessageType>(static_cast<MessageType>(raw));
}

std::string_view to_string(ClientRole role) noexcept {
  switch (role) {
    case ClientRole::Publisher: return "PUBLISHER";
    case ClientRole::Client: return "CLIENT";
    case ClientRole::Admin: return "ADMIN";
  }
  return "PUBLISHER";
}

std::string_view to_string(AckStatus status) noexcept {
  switch (status) {
    case AckStatus::Accepted: return "ACCEPTED";
    case AckStatus::PartiallyAccepted: return "PARTIALLY_ACCEPTED";
    case AckStatus::Rejected: return "REJECTED";
    case AckStatus::Unauthorized: return "UNAUTHORIZED";
    case AckStatus::StaleEpoch: return "STALE_EPOCH";
    case AckStatus::StaleBoot: return "STALE_BOOT";
    case AckStatus::StaleGeneration: return "STALE_GENERATION";
    case AckStatus::StaleSequence: return "STALE_SEQUENCE";
    case AckStatus::Malformed: return "MALFORMED";
    case AckStatus::Capacity: return "CAPACITY";
    case AckStatus::Unsupported: return "UNSUPPORTED";
    case AckStatus::InternalError: return "INTERNAL_ERROR";
  }
  return "INTERNAL_ERROR";
}

AckStatus ack_status_for(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return AckStatus::Accepted;
    case ErrorCode::Unauthorized: return AckStatus::Unauthorized;
    case ErrorCode::StaleEpoch: return AckStatus::StaleEpoch;
    case ErrorCode::StaleBoot: return AckStatus::StaleBoot;
    case ErrorCode::StaleGeneration: return AckStatus::StaleGeneration;
    case ErrorCode::StaleSequence: return AckStatus::StaleSequence;
    case ErrorCode::RetiredEntity: return AckStatus::StaleGeneration;
    case ErrorCode::Capacity:
    case ErrorCode::TooMany: return AckStatus::Capacity;
    case ErrorCode::UnsupportedCapability: return AckStatus::Unsupported;
    case ErrorCode::InvalidMagic:
    case ErrorCode::UnsupportedVersion:
    case ErrorCode::UnknownMessageType:
    case ErrorCode::InvalidFlags:
    case ErrorCode::Truncated:
    case ErrorCode::IntegrityFailure:
    case ErrorCode::ProtocolError:
    case ErrorCode::TrailingBytes:
    case ErrorCode::InvalidArgument:
    case ErrorCode::OutOfRange:
    case ErrorCode::TooLarge:
    case ErrorCode::Conflict:
    case ErrorCode::NotFound:
    case ErrorCode::AlreadyExists:
    case ErrorCode::Duplicate:
    case ErrorCode::Overflow:
    case ErrorCode::Saturated:
    case ErrorCode::Busy:
    case ErrorCode::Shutdown:
    case ErrorCode::IoError:
    case ErrorCode::ConnectionClosed:
    case ErrorCode::Internal:
    case ErrorCode::PrecisionInsufficient:
    case ErrorCode::CorruptState:
    case ErrorCode::UnsupportedStateVersion:
    case ErrorCode::EmptyState:
      return AckStatus::Malformed;
  }
  return AckStatus::InternalError;
}

// ---- PayloadWriter -----------------------------------------------------

void PayloadWriter::u8(std::uint8_t value) { data_.push_back(value); }

void PayloadWriter::u8(bool value) { data_.push_back(value ? std::uint8_t{1} : std::uint8_t{0}); }

void PayloadWriter::u16(std::uint16_t value) {
  data_.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  data_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void PayloadWriter::u32(std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    data_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
  }
}

void PayloadWriter::u64(std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    data_.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
  }
}

void PayloadWriter::i64(std::int64_t value) {
  u64(static_cast<std::uint64_t>(value));
}

void PayloadWriter::f64(double value) {
  std::uint64_t bits = 0;
  static_assert(sizeof(bits) == sizeof(value), "double must be 64 bits");
  std::memcpy(&bits, &value, sizeof(bits));
  u64(bits);
}

void PayloadWriter::bytes(const std::uint8_t* data, std::size_t size) {
  data_.insert(data_.end(), data, data + size);
}

Status PayloadWriter::text(std::string_view value, std::size_t max_length) {
  if (value.size() > max_length) {
    return fail(ErrorCode::TooLarge, "text field exceeds its bound",
                detail::format_u64(value.size()));
  }
  u32(static_cast<std::uint32_t>(value.size()));
  data_.insert(data_.end(), value.begin(), value.end());
  return Status();
}

// ---- PayloadReader -----------------------------------------------------

Status PayloadReader::u8(std::uint8_t* out) {
  if (remaining() < 1) {
    return fail(ErrorCode::Truncated, "payload exhausted reading u8");
  }
  *out = data_[position_++];
  return Status();
}

Status PayloadReader::u16(std::uint16_t* out) {
  if (remaining() < 2) {
    return fail(ErrorCode::Truncated, "payload exhausted reading u16");
  }
  *out = get_u16(data_ + position_);
  position_ += 2;
  return Status();
}

Status PayloadReader::u32(std::uint32_t* out) {
  if (remaining() < 4) {
    return fail(ErrorCode::Truncated, "payload exhausted reading u32");
  }
  *out = get_u32(data_ + position_);
  position_ += 4;
  return Status();
}

Status PayloadReader::u64(std::uint64_t* out) {
  if (remaining() < 8) {
    return fail(ErrorCode::Truncated, "payload exhausted reading u64");
  }
  *out = get_u64(data_ + position_);
  position_ += 8;
  return Status();
}

Status PayloadReader::i64(std::int64_t* out) {
  std::uint64_t raw = 0;
  const Status status = u64(&raw);
  if (!status.ok()) {
    return status;
  }
  *out = static_cast<std::int64_t>(raw);
  return Status();
}

Status PayloadReader::f64(double* out) {
  std::uint64_t raw = 0;
  const Status status = u64(&raw);
  if (!status.ok()) {
    return status;
  }
  std::memcpy(out, &raw, sizeof(raw));
  return Status();
}

Status PayloadReader::bytes(std::uint8_t* out, std::size_t count) {
  if (remaining() < count) {
    return fail(ErrorCode::Truncated, "payload exhausted reading bytes",
                detail::format_u64(count));
  }
  if (count != 0) {
    std::memcpy(out, data_ + position_, count);
    position_ += count;
  }
  return Status();
}

Status PayloadReader::text(std::string* out, std::size_t max_length) {
  std::uint32_t length = 0;
  const Status status = u32(&length);
  if (!status.ok()) {
    return status;
  }
  if (length > max_length) {
    return fail(ErrorCode::TooLarge, "text field exceeds its bound",
                detail::format_u64(length));
  }
  if (remaining() < length) {
    return fail(ErrorCode::Truncated, "payload exhausted reading text");
  }
  out->assign(reinterpret_cast<const char*>(data_ + position_), length);
  position_ += length;
  return Status();
}

Status PayloadReader::require_exhausted() const {
  if (!exhausted()) {
    return fail(ErrorCode::TrailingBytes, "trailing bytes in payload",
                detail::format_u64(remaining()));
  }
  return Status();
}

}  // namespace sol::coherence
