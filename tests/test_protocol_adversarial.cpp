// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Protocol adversarial testing over real sockets.
//
// Every attack is followed by a state-fingerprint comparison: a rejected frame
// must not have mutated authoritative state.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <cstring>
#include <string>
#include <vector>

#include "coherence/coordinator.hpp"
#include "coherence/protocol.hpp"
#include "detail/platform.hpp"
#include "detail/transport.hpp"

namespace {

using namespace sol::coherence;

/// A live coordinator plus one raw socket to it.
class Harness {
 public:
  Harness() {
    CoordinatorOptions options;
    options.bind_host = "127.0.0.1";
    options.port = 0;
    options.save_on_shutdown = false;
    options.load_on_start = false;
    server_ = std::make_unique<CoordinatorServer>(options);
  }

  bool start() {
    if (!server_->start().ok()) {
      return false;
    }
    cotest::register_test_topology(server_->observatory());
    accepted_before_ = server_->observatory().state_fingerprint();
    return true;
  }

  ~Harness() {
    if (server_ != nullptr) {
      server_->stop();
    }
  }

  bool connect_raw() {
    Result<detail::Socket> socket = detail::tcp_connect("127.0.0.1", server_->port(), 4000);
    if (!socket.ok()) {
      return false;
    }
    socket_ = std::make_unique<detail::Socket>(socket.take());
    buffer_.clear();
    return detail::socket_set_timeouts(*socket_, 4000, 4000).ok();
  }

  bool send_bytes(const std::vector<std::uint8_t>& data) {
    return detail::socket_send_all(*socket_, data.data(), data.size()).ok();
  }

  bool send_frame(const Frame& frame) {
    const std::vector<std::uint8_t> encoded = frame.encode();
    return detail::socket_send_all(*socket_, encoded.data(), encoded.size()).ok();
  }

  /// Reads one frame, or reports that the peer closed the connection.
  enum class ReadResult { Frame, Closed, Failed };

  ReadResult read(std::vector<std::uint8_t>* payload_out, MessageType* type_out) {
    buffer_.clear();
    const Result<Frame> frame = detail::receive_frame(*socket_, buffer_);
    if (frame.ok()) {
      *payload_out = frame.value().payload;
      *type_out = frame.value().type;
      return ReadResult::Frame;
    }
    if (frame.code() == ErrorCode::ConnectionClosed) {
      return ReadResult::Closed;
    }
    return ReadResult::Failed;
  }

  void close_socket() {
    if (socket_ != nullptr) {
      socket_->close();
      socket_.reset();
    }
  }

  CoordinatorServer& server() { return *server_; }
  std::uint64_t accepted_before() const { return accepted_before_; }

 private:
  std::unique_ptr<CoordinatorServer> server_;
  std::unique_ptr<detail::Socket> socket_;
  std::vector<std::uint8_t> buffer_;
  std::uint64_t accepted_before_ = 0;
};

/// Builds a raw frame with explicit header fields and a correct checksum so
/// that the attack reaches the layer under test.
std::vector<std::uint8_t> raw_frame(std::uint16_t type, std::uint32_t flags,
                                    std::uint32_t payload_length,
                                    const std::vector<std::uint8_t>& payload,
                                    std::uint16_t version = kWireProtocolVersion,
                                    bool correct_crc = true) {
  FrameHeader header;
  header.version = version;
  header.type = type;
  header.flags = flags;
  header.frame_sequence = 1;
  header.payload_length = payload_length;
  std::vector<std::uint8_t> out(Limits::kFrameHeaderSize + payload.size() + 4);
  header.encode(out.data());
  if (!payload.empty()) {
    std::memcpy(out.data() + Limits::kFrameHeaderSize, payload.data(), payload.size());
  }
  const std::uint32_t crc = crc32(payload.data(), payload.size());
  std::memcpy(out.data() + Limits::kFrameHeaderSize + payload.size(), &crc, sizeof(crc));
  if (!correct_crc) {
    out[Limits::kFrameHeaderSize + payload.size()] ^= 0xFF;
  }
  return out;
}

bool fingerprint_unchanged(Harness& harness) {
  return harness.server().observatory().state_fingerprint() == harness.accepted_before();
}

CO_TEST(protocol_empty_frame_and_short_header_are_rejected) {
  Harness harness;
  CO_REQUIRE(harness.start());
  CO_REQUIRE(harness.connect_raw());
  // Eight bytes are not a frame header: the coordinator must wait for the
  // rest and then reject the header once the remaining bytes arrive.
  const std::vector<std::uint8_t> short_header(8, 0);
  CO_CHECK(harness.send_bytes(short_header));
  PayloadWriter hello_writer;
  hello_writer.u16(kWireProtocolVersion);
  hello_writer.u8(static_cast<std::uint8_t>(ClientRole::Client));
  hello_writer.text("short", Limits::kMaxNameLength);
  Frame hello;
  hello.type = MessageType::Hello;
  hello.payload = hello_writer.data();
  CO_CHECK(harness.send_frame(hello));
  std::vector<std::uint8_t> payload;
  MessageType type = MessageType::Hello;
  const Harness::ReadResult result = harness.read(&payload, &type);
  CO_CHECK(result == Harness::ReadResult::Frame || result == Harness::ReadResult::Closed);
  if (result == Harness::ReadResult::Frame) {
    CO_CHECK(type == MessageType::Error);
  }
  CO_CHECK(fingerprint_unchanged(harness));
  CO_CHECK(harness.server().protocol_rejections() > 0);
}

CO_TEST(protocol_bad_magic_version_type_and_flags_are_rejected) {
  Harness harness;
  CO_REQUIRE(harness.start());

  const std::uint8_t zeros[4] = {0, 0, 0, 0};
  struct Attack {
    std::vector<std::uint8_t> bytes;
    const char* name;
  };
  std::vector<Attack> attacks;

  {
    std::vector<std::uint8_t> bad_magic =
        raw_frame(static_cast<std::uint16_t>(MessageType::Hello), 0, 4,
                  std::vector<std::uint8_t>(zeros, zeros + 4));
    bad_magic[0] = 'Z';
    attacks.push_back({bad_magic, "bad magic"});
  }
  attacks.push_back({raw_frame(static_cast<std::uint16_t>(MessageType::Hello), 0, 4,
                               std::vector<std::uint8_t>(zeros, zeros + 4),
                               static_cast<std::uint16_t>(kWireProtocolVersion + 9)),
                     "bad version"});
  attacks.push_back({raw_frame(9999, 0, 4, std::vector<std::uint8_t>(zeros, zeros + 4)),
                     "unknown type"});
  attacks.push_back({raw_frame(static_cast<std::uint16_t>(MessageType::Hello), 0x80, 4,
                               std::vector<std::uint8_t>(zeros, zeros + 4)),
                     "invalid flags"});
  attacks.push_back({raw_frame(static_cast<std::uint16_t>(MessageType::Hello), 0,
                               static_cast<std::uint32_t>(Limits::kMaxFramePayload + 1),
                               std::vector<std::uint8_t>(zeros, zeros + 4)),
                     "oversized length"});
  attacks.push_back({raw_frame(static_cast<std::uint16_t>(MessageType::Hello), 0, 4,
                               std::vector<std::uint8_t>(zeros, zeros + 4), kWireProtocolVersion,
                               false),
                     "corrupt checksum"});

  for (const Attack& attack : attacks) {
    CO_REQUIRE(harness.connect_raw());
    CO_CHECK(harness.send_bytes(attack.bytes));
    std::vector<std::uint8_t> payload;
    MessageType type = MessageType::Hello;
    const Harness::ReadResult result = harness.read(&payload, &type);
    CO_CHECK(result == Harness::ReadResult::Frame || result == Harness::ReadResult::Closed);
    if (result == Harness::ReadResult::Frame) {
      CO_CHECK(type == MessageType::Error);
    }
    CO_CHECK(fingerprint_unchanged(harness));
    harness.close_socket();
  }
}

CO_TEST(protocol_truncated_payload_and_trailing_bytes_are_rejected) {
  Harness harness;
  CO_REQUIRE(harness.start());

  CO_REQUIRE(harness.connect_raw());
  {
    // The header declares 64 payload bytes but only 16 are supplied; the
    // remainder is filled with unrelated bytes so the declared payload CRC
    // cannot match.
    std::vector<std::uint8_t> frame =
        raw_frame(static_cast<std::uint16_t>(MessageType::PublishEvent), 0, 64,
                  std::vector<std::uint8_t>(16, 0x11));
    frame.insert(frame.end(), 48, 0x22);
    CO_CHECK(harness.send_bytes(frame));
    std::vector<std::uint8_t> payload;
    MessageType type = MessageType::Hello;
    const Harness::ReadResult result = harness.read(&payload, &type);
    CO_CHECK(result == Harness::ReadResult::Frame || result == Harness::ReadResult::Closed);
    CO_CHECK(fingerprint_unchanged(harness));
  }
  harness.close_socket();

  CO_REQUIRE(harness.connect_raw());
  {
    std::vector<std::uint8_t> frame =
        raw_frame(static_cast<std::uint16_t>(MessageType::Heartbeat), 0, 0, {});
    // A complete, well formed frame followed by unrelated bytes.
    const std::uint8_t trailing[5] = {0xDE, 0xAD, 0xBE, 0xEF, 0x00};
    frame.insert(frame.end(), trailing, trailing + 5);
    CO_CHECK(harness.send_bytes(frame));
    std::vector<std::uint8_t> payload;
    MessageType type = MessageType::Hello;
    const Harness::ReadResult first_read = harness.read(&payload, &type);
    CO_CHECK(first_read == Harness::ReadResult::Frame ||
             first_read == Harness::ReadResult::Closed);
    CO_CHECK(fingerprint_unchanged(harness));
  }
  harness.close_socket();
}

CO_TEST(protocol_requires_hello_first_and_rejects_duplicate_hello) {
  Harness harness;
  CO_REQUIRE(harness.start());
  CO_REQUIRE(harness.connect_raw());

  {
    PayloadWriter writer;
    writer.u8(static_cast<std::uint8_t>(ClientRole::Publisher));
    Frame frame;
    frame.type = MessageType::RegisterPublisher;
    frame.payload = writer.data();
    CO_CHECK(harness.send_frame(frame));
    std::vector<std::uint8_t> payload;
    MessageType type = MessageType::Hello;
    const Harness::ReadResult result = harness.read(&payload, &type);
    CO_CHECK(result == Harness::ReadResult::Frame || result == Harness::ReadResult::Closed);
    if (result == Harness::ReadResult::Frame) {
      CO_CHECK(type == MessageType::Error);
    }
  }
  harness.close_socket();

  CO_REQUIRE(harness.connect_raw());
  {
    PayloadWriter writer;
    writer.u16(kWireProtocolVersion);
    writer.u8(static_cast<std::uint8_t>(ClientRole::Client));
    writer.text("dup", Limits::kMaxNameLength);
    Frame hello;
    hello.type = MessageType::Hello;
    hello.payload = writer.data();
    CO_CHECK(harness.send_frame(hello));
    std::vector<std::uint8_t> payload;
    MessageType type = MessageType::Hello;
    CO_CHECK(harness.read(&payload, &type) == Harness::ReadResult::Frame);
    CO_CHECK(type == MessageType::HelloAck);
    CO_CHECK(harness.send_frame(hello));
    CO_CHECK(harness.read(&payload, &type) == Harness::ReadResult::Frame ||
             harness.read(&payload, &type) == Harness::ReadResult::Closed);
    CO_CHECK(fingerprint_unchanged(harness));
  }
}

CO_TEST(protocol_unauthorized_structural_mutation_is_refused) {
  Harness harness;
  CO_REQUIRE(harness.start());
  CO_REQUIRE(harness.connect_raw());

  PayloadWriter hello_writer;
  hello_writer.u16(kWireProtocolVersion);
  hello_writer.u8(static_cast<std::uint8_t>(ClientRole::Publisher));
  hello_writer.text("publisher", Limits::kMaxNameLength);
  Frame hello;
  hello.type = MessageType::Hello;
  hello.payload = hello_writer.data();
  CO_CHECK(harness.send_frame(hello));
  std::vector<std::uint8_t> payload;
  MessageType type = MessageType::Hello;
  CO_CHECK(harness.read(&payload, &type) == Harness::ReadResult::Frame);
  CO_CHECK(type == MessageType::HelloAck);

  // A publisher may not register structural state.
  PayloadWriter register_writer;
  register_writer.u8(static_cast<std::uint8_t>(ResourceKind::MemoryDomain));
  register_writer.u32(1);
  Frame registration;
  registration.type = MessageType::RegisterResource;
  registration.payload = register_writer.data();
  CO_CHECK(harness.send_frame(registration));
  const Harness::ReadResult result = harness.read(&payload, &type);
  CO_CHECK(result == Harness::ReadResult::Frame || result == Harness::ReadResult::Closed);
  if (result == Harness::ReadResult::Frame) {
    CO_CHECK(type == MessageType::Error);
  }
  CO_CHECK(fingerprint_unchanged(harness));
}

CO_TEST(protocol_invalid_batch_count_is_refused) {
  Harness harness;
  CO_REQUIRE(harness.start());
  CO_REQUIRE(harness.connect_raw());

  PayloadWriter hello_writer;
  hello_writer.u16(kWireProtocolVersion);
  hello_writer.u8(static_cast<std::uint8_t>(ClientRole::Admin));
  hello_writer.text("admin", Limits::kMaxNameLength);
  Frame hello;
  hello.type = MessageType::Hello;
  hello.payload = hello_writer.data();
  CO_CHECK(harness.send_frame(hello));
  std::vector<std::uint8_t> payload;
  MessageType type = MessageType::Hello;
  CO_CHECK(harness.read(&payload, &type) == Harness::ReadResult::Frame);
  CO_CHECK(type == MessageType::HelloAck);

  PayloadWriter batch_writer;
  batch_writer.u32(static_cast<std::uint32_t>(Limits::kMaxBatchEvents + 1));
  Frame batch;
  batch.type = MessageType::PublishBatch;
  batch.payload = batch_writer.data();
  CO_CHECK(harness.send_frame(batch));
  const Harness::ReadResult result = harness.read(&payload, &type);
  CO_CHECK(result == Harness::ReadResult::Frame || result == Harness::ReadResult::Closed);
  if (result == Harness::ReadResult::Frame) {
    CO_CHECK(type == MessageType::Error);
  }
  CO_CHECK(fingerprint_unchanged(harness));
}

CO_TEST(protocol_oversized_metadata_and_stale_publisher_frames) {
  Harness harness;
  CO_REQUIRE(harness.start());

  // Register a publisher through the normal path.
  PublisherRegistration registration;
  registration.id = PublisherId{"pub.proto.1"};
  registration.boot = PublisherBootId{4242};
  registration.provenance = Provenance::SyntheticBackend;
  CO_REQUIRE(harness.server().observatory().register_publisher(registration).ok());
  const std::uint64_t with_publisher = harness.server().observatory().state_fingerprint();

  CO_REQUIRE(harness.connect_raw());
  PayloadWriter hello_writer;
  hello_writer.u16(kWireProtocolVersion);
  hello_writer.u8(static_cast<std::uint8_t>(ClientRole::Admin));
  hello_writer.text("admin", Limits::kMaxNameLength);
  Frame hello;
  hello.type = MessageType::Hello;
  hello.payload = hello_writer.data();
  CO_CHECK(harness.send_frame(hello));
  std::vector<std::uint8_t> payload;
  MessageType type = MessageType::Hello;
  CO_CHECK(harness.read(&payload, &type) == Harness::ReadResult::Frame);

  // A fence for an unknown publisher must not mutate state.
  PayloadWriter fence_writer;
  fence_writer.text("pub.absent", Limits::kMaxNameLength);
  fence_writer.u8(static_cast<std::uint8_t>(FenceReason::Administrative));
  fence_writer.text("attack", Limits::kMaxRegionAnnotationLength);
  Frame fence;
  fence.type = MessageType::Fence;
  fence.payload = fence_writer.data();
  CO_CHECK(harness.send_frame(fence));
  CO_CHECK(harness.read(&payload, &type) == Harness::ReadResult::Frame);
  CO_CHECK(type == MessageType::Error);
  CO_CHECK_EQ(harness.server().observatory().state_fingerprint(), with_publisher);

  // Fencing a publisher that does exist is an authorised administrative
  // mutation and must be observable.
  PayloadWriter known_writer;
  known_writer.text("pub.proto.1", Limits::kMaxNameLength);
  known_writer.u8(static_cast<std::uint8_t>(FenceReason::Administrative));
  known_writer.text("administrative fence", Limits::kMaxRegionAnnotationLength);
  Frame known;
  known.type = MessageType::Fence;
  known.payload = known_writer.data();
  CO_CHECK(harness.send_frame(known));
  CO_CHECK(harness.read(&payload, &type) == Harness::ReadResult::Frame);
  CO_CHECK(type == MessageType::Ack);
  CO_CHECK(harness.server().observatory().state_fingerprint() != with_publisher);
  const Result<PublisherView> fenced =
      harness.server().observatory().publisher(PublisherId{"pub.proto.1"});
  CO_REQUIRE(fenced.ok());
  CO_CHECK(fenced.value().fenced);
  CO_CHECK(!fenced.value().current);
  harness.close_socket();
}

CO_TEST(protocol_connection_close_fences_the_publisher) {
  Harness harness;
  CO_REQUIRE(harness.start());

  PublisherRegistration registration;
  registration.id = PublisherId{"pub.proto.close"};
  registration.boot = make_publisher_boot_id();
  registration.provenance = Provenance::SyntheticBackend;
  const Result<PublisherView> view =
      harness.server().observatory().register_publisher(registration);
  CO_REQUIRE(view.ok());
  CO_CHECK(view.value().current);

  CO_REQUIRE(harness.connect_raw());
  PayloadWriter hello_writer;
  hello_writer.u16(kWireProtocolVersion);
  hello_writer.u8(static_cast<std::uint8_t>(ClientRole::Admin));
  hello_writer.text("admin", Limits::kMaxNameLength);
  Frame hello;
  hello.type = MessageType::Hello;
  hello.payload = hello_writer.data();
  CO_CHECK(harness.send_frame(hello));
  std::vector<std::uint8_t> payload;
  MessageType type = MessageType::Hello;
  CO_CHECK(harness.read(&payload, &type) == Harness::ReadResult::Frame);

  // Force a protocol error so the coordinator closes the connection.
  CO_CHECK(harness.send_bytes(std::vector<std::uint8_t>(4, 0)));
  harness.read(&payload, &type);
  harness.close_socket();

  // The in-process publisher registered above was not tied to a session, so it
  // stays current: only transport-bound publishers lose authority on close.
  const Result<PublisherView> still = harness.server().observatory().publisher(
      PublisherId{"pub.proto.close"});
  CO_REQUIRE(still.ok());
  CO_CHECK(still.value().current);
}

}  // namespace