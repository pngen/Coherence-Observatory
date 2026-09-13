// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "test_framework.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "coherence/protocol.hpp"

namespace {

using namespace sol::coherence;

Frame make_frame(MessageType type, std::size_t payload_size) {
  Frame frame;
  frame.type = type;
  frame.frame_sequence = 42;
  frame.payload.assign(payload_size, 0x5Au);
  return frame;
}

CO_TEST(frame_round_trip) {
  const Frame original = make_frame(MessageType::PublishEvent, 128);
  const std::vector<std::uint8_t> encoded = original.encode();
  CO_CHECK_EQ(encoded.size(),
              Limits::kFrameHeaderSize + 128 + Limits::kFrameTrailerSize);
  const Result<Frame> decoded = Frame::decode(encoded.data(), encoded.size());
  CO_REQUIRE(decoded.ok());
  CO_CHECK(decoded.value().type == MessageType::PublishEvent);
  CO_CHECK_EQ(decoded.value().frame_sequence, std::uint64_t{42});
  CO_CHECK(decoded.value().payload == original.payload);
}

CO_TEST(frame_rejects_bad_magic) {
  std::vector<std::uint8_t> encoded = make_frame(MessageType::Hello, 0).encode();
  encoded[0] = 'X';
  const Result<Frame> decoded = Frame::decode(encoded.data(), encoded.size());
  CO_CHECK(!decoded.ok());
  CO_CHECK(decoded.code() == ErrorCode::InvalidMagic);
}

CO_TEST(frame_rejects_short_header_and_truncation) {
  const std::vector<std::uint8_t> encoded = make_frame(MessageType::Hello, 16).encode();
  const Result<Frame> short_header = Frame::decode(encoded.data(), 8);
  CO_CHECK(!short_header.ok());
  CO_CHECK(short_header.code() == ErrorCode::Truncated);
  const Result<Frame> truncated =
      Frame::decode(encoded.data(), encoded.size() - 1);
  CO_CHECK(!truncated.ok());
  CO_CHECK(truncated.code() == ErrorCode::Truncated);
}

CO_TEST(frame_rejects_trailing_bytes) {
  std::vector<std::uint8_t> encoded = make_frame(MessageType::Hello, 4).encode();
  encoded.push_back(0xFF);
  const Result<Frame> decoded = Frame::decode(encoded.data(), encoded.size());
  CO_CHECK(!decoded.ok());
  CO_CHECK(decoded.code() == ErrorCode::TrailingBytes);
}

CO_TEST(frame_rejects_unsupported_version) {
  std::vector<std::uint8_t> encoded = make_frame(MessageType::Hello, 0).encode();
  encoded[8] = 99;
  encoded[9] = 0;
  const Result<Frame> decoded = Frame::decode(encoded.data(), encoded.size());
  CO_CHECK(!decoded.ok());
  CO_CHECK(decoded.code() == ErrorCode::IntegrityFailure ||
           decoded.code() == ErrorCode::UnsupportedVersion);
}

CO_TEST(frame_rejects_unknown_type_and_flags) {
  std::vector<std::uint8_t> unknown = make_frame(MessageType::Hello, 0).encode();
  unknown[10] = 0xEE;
  unknown[11] = 0xEE;
  const Result<Frame> unknown_decoded = Frame::decode(unknown.data(), unknown.size());
  CO_CHECK(!unknown_decoded.ok());

  std::vector<std::uint8_t> flags = make_frame(MessageType::Hello, 0).encode();
  flags[12] = 0x80;
  const Result<Frame> flags_decoded = Frame::decode(flags.data(), flags.size());
  CO_CHECK(!flags_decoded.ok());
  CO_CHECK(flags_decoded.code() == ErrorCode::IntegrityFailure ||
           flags_decoded.code() == ErrorCode::InvalidFlags);
}

CO_TEST(frame_rejects_oversized_and_corrupt_payload) {
  std::vector<std::uint8_t> oversized = make_frame(MessageType::Hello, 0).encode();
  const std::uint32_t too_large =
      static_cast<std::uint32_t>(Limits::kMaxFramePayload + 1);
  std::memcpy(oversized.data() + 24, &too_large, sizeof(too_large));
  const Result<Frame> oversized_decoded =
      Frame::decode(oversized.data(), oversized.size());
  CO_CHECK(!oversized_decoded.ok());

  std::vector<std::uint8_t> corrupt = make_frame(MessageType::Hello, 32).encode();
  corrupt[Limits::kFrameHeaderSize + 3] ^= 0xFF;
  const Result<Frame> corrupt_decoded = Frame::decode(corrupt.data(), corrupt.size());
  CO_CHECK(!corrupt_decoded.ok());
  CO_CHECK(corrupt_decoded.code() == ErrorCode::IntegrityFailure);
}

CO_TEST(header_checksum_covers_the_whole_header) {
  std::vector<std::uint8_t> encoded = make_frame(MessageType::Hello, 0).encode();
  encoded[12] = 0x01;  // set the ack-required flag without recomputing the CRC
  const Result<Frame> decoded = Frame::decode(encoded.data(), encoded.size());
  CO_CHECK(!decoded.ok());
  CO_CHECK(decoded.code() == ErrorCode::IntegrityFailure);
}

CO_TEST(payload_writer_reader_bounds) {
  PayloadWriter writer;
  writer.u8(std::uint8_t{1});
  writer.u16(2);
  writer.u32(3);
  writer.u64(4);
  writer.i64(-5);
  writer.f64(1.5);
  CO_CHECK(writer.text("hello", 8).ok());
  CO_CHECK(!writer.text(std::string(9, 'x'), 8).ok());

  PayloadReader reader(writer.data().data(), writer.data().size());
  std::uint8_t a = 0;
  std::uint16_t b = 0;
  std::uint32_t c = 0;
  std::uint64_t d = 0;
  std::int64_t e = 0;
  double f = 0.0;
  std::string text;
  CO_CHECK(reader.u8(&a).ok());
  CO_CHECK(reader.u16(&b).ok());
  CO_CHECK(reader.u32(&c).ok());
  CO_CHECK(reader.u64(&d).ok());
  CO_CHECK(reader.i64(&e).ok());
  CO_CHECK(reader.f64(&f).ok());
  CO_CHECK(reader.text(&text, 8).ok());
  CO_CHECK_EQ(a, std::uint8_t{1});
  CO_CHECK_EQ(b, std::uint16_t{2});
  CO_CHECK_EQ(c, std::uint32_t{3});
  CO_CHECK_EQ(d, std::uint64_t{4});
  CO_CHECK_EQ(e, std::int64_t{-5});
  CO_CHECK(f > 1.49 && f < 1.51);
  CO_CHECK(text == "hello");
  CO_CHECK(reader.require_exhausted().ok());

  PayloadReader short_reader(writer.data().data(), 2);
  CO_CHECK(!short_reader.u32(&c).ok());

  std::vector<std::uint8_t> trailing = writer.data();
  trailing.push_back(0);
  PayloadReader trailing_reader(trailing.data(), trailing.size());
  CO_CHECK(!trailing_reader.require_exhausted().ok());
}

CO_TEST(crc32_matches_known_values) {
  const std::uint8_t empty[1] = {0};
  CO_CHECK_EQ(crc32(empty, 0), std::uint32_t{0});
  const std::string check = "123456789";
  CO_CHECK_EQ(crc32(reinterpret_cast<const std::uint8_t*>(check.data()), check.size()),
              std::uint32_t{0xCBF43926u});
}

CO_TEST(ack_status_mapping_is_total) {
  CO_CHECK(ack_status_for(ErrorCode::Ok) == AckStatus::Accepted);
  CO_CHECK(ack_status_for(ErrorCode::Unauthorized) == AckStatus::Unauthorized);
  CO_CHECK(ack_status_for(ErrorCode::StaleEpoch) == AckStatus::StaleEpoch);
  CO_CHECK(ack_status_for(ErrorCode::StaleBoot) == AckStatus::StaleBoot);
  CO_CHECK(ack_status_for(ErrorCode::StaleGeneration) == AckStatus::StaleGeneration);
  CO_CHECK(ack_status_for(ErrorCode::StaleSequence) == AckStatus::StaleSequence);
  CO_CHECK(ack_status_for(ErrorCode::TooMany) == AckStatus::Capacity);
  CO_CHECK(ack_status_for(ErrorCode::UnsupportedCapability) == AckStatus::Unsupported);
  CO_CHECK(ack_status_for(ErrorCode::InvalidMagic) == AckStatus::Malformed);
  CO_CHECK(ack_status_for(ErrorCode::Internal) == AckStatus::Malformed);
  CO_CHECK(parse_message_type(static_cast<std::uint16_t>(MessageType::PublishBatch)).ok());
  CO_CHECK(!parse_message_type(9999).ok());
}

}  // namespace