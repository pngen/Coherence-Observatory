// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Remote clients.  Every client is synchronous: one request, one response.
// There is no background reader thread, so no client-side lock is ever held
// across a socket operation and no shutdown path can self-join.

#include "coherence/client.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

#include "coherence/checked.hpp"
#include "detail/platform.hpp"
#if defined(_WIN32)
#include <process.h>
#endif
#include "detail/transport.hpp"
#include "detail/wire.hpp"
#include "text_util.hpp"

namespace sol::coherence {
namespace {

using detail::AckPayload;
using detail::Socket;

/// Flags byte for query messages.
std::uint8_t findings_flags(const FindingsOptions& options) {
  std::uint8_t flags = 0;
  if (options.compute_hotspots) flags |= 0x01u;
  if (options.compute_ping_pong) flags |= 0x02u;
  if (options.compute_false_sharing) flags |= 0x04u;
  if (options.compute_invalidation) flags |= 0x08u;
  if (options.compute_remote_access) flags |= 0x10u;
  if (options.include_operational) flags |= 0x20u;
  return flags;
}

FindingsOptions findings_options_from_flags(std::uint8_t flags) {
  FindingsOptions options;
  options.compute_hotspots = (flags & 0x01u) != 0;
  options.compute_ping_pong = (flags & 0x02u) != 0;
  options.compute_false_sharing = (flags & 0x04u) != 0;
  options.compute_invalidation = (flags & 0x08u) != 0;
  options.compute_remote_access = (flags & 0x10u) != 0;
  options.include_operational = (flags & 0x20u) != 0;
  return options;
}

struct HelloInfo {
  CoordinatorEpoch coordinator_epoch;
  ObservationEpoch observation_epoch;
  TopologyGeneration topology_generation;
  std::string server_name;
  std::string endpoint;
  ClientRole role = ClientRole::Publisher;
};

Status send_hello(const Socket& socket, ClientRole role, const std::string& name,
                  std::vector<std::uint8_t>& buffer, HelloInfo* info) {
  Frame hello;
  hello.type = MessageType::Hello;
  PayloadWriter writer;
  writer.u16(kWireProtocolVersion);
  writer.u8(static_cast<std::uint8_t>(role));
  Status status = writer.text(name, Limits::kMaxNameLength);
  if (!status.ok()) {
    return status;
  }
  hello.payload = writer.data();
  status = detail::send_frame(socket, hello);
  if (!status.ok()) {
    return status;
  }
  Result<Frame> response = detail::receive_frame(socket, buffer);
  if (!response.ok()) {
    return Status(response.error());
  }
  if (response.value().type != MessageType::HelloAck) {
    return fail(ErrorCode::ProtocolError, "coordinator did not acknowledge HELLO");
  }
  PayloadReader reader(response.value().payload.data(), response.value().payload.size());
  std::uint16_t version = 0;
  std::uint64_t coordinator = 0;
  std::uint64_t observation = 0;
  std::uint8_t role_value = 0;
  status = reader.u16(&version);
  if (!status.ok()) return status;
  status = reader.u64(&coordinator);
  if (!status.ok()) return status;
  status = reader.u64(&observation);
  if (!status.ok()) return status;
  std::uint64_t topology = 0;
  status = reader.u64(&topology);
  if (!status.ok()) return status;
  status = reader.text(&info->server_name, Limits::kMaxNameLength);
  if (!status.ok()) return status;
  status = reader.text(&info->endpoint, Limits::kMaxRegionAnnotationLength);
  if (!status.ok()) return status;
  status = reader.u8(&role_value);
  if (!status.ok()) return status;
  status = reader.require_exhausted();
  if (!status.ok()) return status;
  if (version != kWireProtocolVersion) {
    return fail(ErrorCode::UnsupportedVersion, "coordinator speaks an unsupported version");
  }
  if (role_value != static_cast<std::uint8_t>(role)) {
    return fail(ErrorCode::Unauthorized, "coordinator refused the requested role");
  }
  info->coordinator_epoch = CoordinatorEpoch{coordinator};
  info->observation_epoch = ObservationEpoch{observation};
  info->topology_generation = TopologyGeneration{topology};
  info->role = role;
  return Status();
}

/// Sends a frame and reads the acknowledgement.
Result<AckPayload> exchange(const Socket& socket, std::vector<std::uint8_t>& buffer,
                            const Frame& request) {
  const Status sent = detail::send_frame(socket, request);
  if (!sent.ok()) {
    return Result<AckPayload>(sent.error());
  }
  Result<Frame> response = detail::receive_frame(socket, buffer);
  if (!response.ok()) {
    return Result<AckPayload>(response.error());
  }
  if (response.value().type != MessageType::Ack &&
      response.value().type != MessageType::Error) {
    return fail_as<AckPayload>(ErrorCode::ProtocolError,
                               "expected an acknowledgement",
                               std::string(to_string(response.value().type)));
  }
  PayloadReader reader(response.value().payload.data(), response.value().payload.size());
  Result<AckPayload> ack = detail::read_ack(reader);
  if (!ack.ok()) {
    return ack;
  }
  const Status exhausted = reader.require_exhausted();
  if (!exhausted.ok()) {
    return Result<AckPayload>(exhausted.error());
  }
  return ack;
}

Status ack_to_status(const AckPayload& ack) {
  if (ack.status == AckStatus::Accepted || ack.status == AckStatus::PartiallyAccepted) {
    return Status();
  }
  ErrorCode code = ErrorCode::Internal;
  switch (ack.status) {
    case AckStatus::Unauthorized: code = ErrorCode::Unauthorized; break;
    case AckStatus::StaleEpoch: code = ErrorCode::StaleEpoch; break;
    case AckStatus::StaleBoot: code = ErrorCode::StaleBoot; break;
    case AckStatus::StaleGeneration: code = ErrorCode::StaleGeneration; break;
    case AckStatus::StaleSequence: code = ErrorCode::StaleSequence; break;
    case AckStatus::Capacity: code = ErrorCode::Capacity; break;
    case AckStatus::Unsupported: code = ErrorCode::UnsupportedCapability; break;
    case AckStatus::Malformed: code = ErrorCode::InvalidArgument; break;
    case AckStatus::Rejected: code = ErrorCode::Conflict; break;
    case AckStatus::PartiallyAccepted:
    case AckStatus::Accepted: code = ErrorCode::Ok; break;
    case AckStatus::InternalError: code = ErrorCode::Internal; break;
  }
  return fail(code, ack.detail);
}

std::uint64_t process_identity() noexcept {
#if defined(_WIN32)
  return static_cast<std::uint64_t>(::_getpid());
#else
  return static_cast<std::uint64_t>(::getpid());
#endif
}

std::uint64_t mix64(std::uint64_t value) noexcept {
  value += 0x9E3779B97F4A7C15ull;
  value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
  value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
  return value ^ (value >> 31);
}

/// Shared connection state.  Members are public to the translation unit: the
/// class is an implementation detail and is never exposed to consumers.
class BaseClient {
 public:
  virtual ~BaseClient() = default;

  Status connect_role(const ClientOptions& options, ClientRole role, std::string name) {
    const Status initialized = detail::network_init();
    if (!initialized.ok()) {
      return initialized;
    }
    Result<Socket> socket =
        detail::tcp_connect(options.host, options.port, options.connect_timeout_ms);
    if (!socket.ok()) {
      return Status(socket.error());
    }
    socket_ = socket.take();
    buffer_.clear();
    options_ = options;
    std::string label = name.empty() ? options.client_name : std::move(name);
    if (label.empty()) {
      label = "coherence-client";
    }
    const Status hello = send_hello(socket_, role, label, buffer_, &hello_);
    if (!hello.ok()) {
      socket_.close();
      return hello;
    }
    return Status();
  }

  Socket socket_;
  std::vector<std::uint8_t> buffer_;
  ClientOptions options_;
  HelloInfo hello_;
  std::uint64_t frame_sequence_ = 0;
};

}  // namespace

PublisherBootId make_publisher_boot_id() noexcept {
  static std::atomic<std::uint64_t> counter{0};
  const std::uint64_t tick =
      static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  const std::uint64_t wall =
      static_cast<std::uint64_t>(std::chrono::system_clock::now().time_since_epoch().count());
  const std::uint64_t sequence = counter.fetch_add(1, std::memory_order_relaxed);
  std::uint64_t value = mix64(tick ^ mix64(wall ^ mix64(process_identity() + sequence)));
  if (value == 0) {
    value = 1;
  }
  return PublisherBootId{value};
}

struct ObservationPublisher::Impl : public BaseClient {
  std::mutex mutex;
  bool connected = false;
  PublisherRegistration registration;
  EventSequence next_sequence{1};
  LossReport loss;
  std::uint64_t rejected = 0;
  std::uint64_t accepted = 0;
};

ObservationPublisher::ObservationPublisher() : impl_(std::make_unique<Impl>()) {}
ObservationPublisher::~ObservationPublisher() { close(); }

Status ObservationPublisher::connect(const ClientOptions& options,
                                     const PublisherRegistration& registration) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->connected) {
    return fail(ErrorCode::AlreadyExists, "publisher is already connected");
  }
  PublisherRegistration effective = registration;
  if (effective.boot.is_zero()) {
    effective.boot = make_publisher_boot_id();
  }
  const Status hello = impl_->connect_role(options, ClientRole::Publisher,
                                           effective.display_name);
  if (!hello.ok()) {
    return hello;
  }

  Frame request;
  request.type = MessageType::RegisterPublisher;
  request.flags = frame_flags::kAckRequired;
  request.frame_sequence = ++impl_->frame_sequence_;
  PayloadWriter writer;
  Status status = detail::write_publisher_registration(writer, effective);
  if (!status.ok()) {
    impl_->socket_.close();
    return status;
  }
  request.payload = writer.data();
  const Result<AckPayload> ack = exchange(impl_->socket_, impl_->buffer_, request);
  if (!ack.ok()) {
    impl_->socket_.close();
    return Status(ack.error());
  }
  const Status registered = ack_to_status(ack.value());
  if (!registered.ok()) {
    impl_->socket_.close();
    return registered;
  }
  impl_->registration = effective;
  impl_->registration.coordinator_epoch = ack.value().coordinator_epoch;
  impl_->connected = true;
  return Status();
}

bool ObservationPublisher::connected() const noexcept {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->connected;
}

CoordinatorEpoch ObservationPublisher::coordinator_epoch() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->registration.coordinator_epoch;
}

TopologyGeneration ObservationPublisher::topology_generation() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->hello_.topology_generation;
}

PublisherId ObservationPublisher::publisher_id() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->registration.id;
}

PublisherBootId ObservationPublisher::publisher_boot() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->registration.boot;
}

EventSequence ObservationPublisher::next_sequence() {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->next_sequence.next();
}

Result<IngestionOutcome> ObservationPublisher::publish(Observation observation) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  IngestionOutcome outcome;
  if (!impl_->connected) {
    outcome.disposition = IngestionDisposition::Rejected;
    outcome.code = ErrorCode::ConnectionClosed;
    outcome.detail = "publisher is not connected";
    return Result<IngestionOutcome>(std::move(outcome));
  }
  if (observation.source_publisher.empty()) {
    observation.source_publisher = impl_->registration.id;
  }
  if (observation.publisher_boot.is_zero()) {
    observation.publisher_boot = impl_->registration.boot;
  }
  observation.coordinator_epoch = impl_->registration.coordinator_epoch;
  if (observation.topology_generation.is_zero()) {
    observation.topology_generation = impl_->hello_.topology_generation;
  }
  if (observation.sampling_epoch.is_zero()) {
    observation.sampling_epoch = impl_->registration.sampling_epoch;
  }
  if (observation.evidence_generation.is_zero()) {
    observation.evidence_generation = impl_->registration.evidence_generation;
  }
  if (observation.event_id.is_zero()) {
    observation.event_id = CoherenceEventId{impl_->next_sequence.value()};
  }
  if (observation.sequence.is_zero()) {
    observation.sequence = impl_->next_sequence.next();
  } else if (observation.sequence >= impl_->next_sequence) {
    impl_->next_sequence = observation.sequence.next();
  }

  Frame request;
  request.type = MessageType::PublishEvent;
  request.flags = frame_flags::kAckRequired;
  request.frame_sequence = ++impl_->frame_sequence_;
  PayloadWriter writer;
  const Status written = detail::write_observation(writer, observation);
  if (!written.ok()) {
    outcome.disposition = IngestionDisposition::Rejected;
    outcome.code = written.code();
    outcome.detail = written.describe();
    return Result<IngestionOutcome>(std::move(outcome));
  }
  request.payload = writer.data();
  const Result<AckPayload> ack = exchange(impl_->socket_, impl_->buffer_, request);
  if (!ack.ok()) {
    impl_->connected = false;
    outcome.disposition = IngestionDisposition::Rejected;
    outcome.code = ack.code();
    outcome.detail = ack.error().describe();
    return Result<IngestionOutcome>(std::move(outcome));
  }
  outcome.detail = ack.value().detail;
  outcome.missing_sequences = ack.value().missing;
  switch (ack.value().status) {
    case AckStatus::Accepted:
      outcome.disposition = IngestionDisposition::Accepted;
      outcome.counted = true;
      ++impl_->accepted;
      break;
    case AckStatus::PartiallyAccepted:
      outcome.disposition = IngestionDisposition::Duplicate;
      outcome.code = ErrorCode::Duplicate;
      impl_->loss.rejected_duplicates += 1;
      ++impl_->rejected;
      break;
    default:
      outcome.disposition = IngestionDisposition::Rejected;
      outcome.code = ack_to_status(ack.value()).code();
      ++impl_->rejected;
      break;
  }
  return Result<IngestionOutcome>(std::move(outcome));
}

Result<CounterOutcome> ObservationPublisher::publish_counter(
    CounterPublication publication) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  CounterOutcome outcome;
  if (!impl_->connected) {
    outcome.code = ErrorCode::ConnectionClosed;
    outcome.detail = "publisher is not connected";
    return Result<CounterOutcome>(std::move(outcome));
  }
  if (publication.publisher.empty()) {
    publication.publisher = impl_->registration.id;
  }
  if (publication.publisher_boot.is_zero()) {
    publication.publisher_boot = impl_->registration.boot;
  }
  publication.coordinator_epoch = impl_->registration.coordinator_epoch;
  if (publication.topology_generation.is_zero()) {
    publication.topology_generation = impl_->hello_.topology_generation;
  }
  if (publication.evidence_generation.is_zero()) {
    publication.evidence_generation = impl_->registration.evidence_generation;
  }
  if (publication.sampling_epoch.is_zero()) {
    publication.sampling_epoch = impl_->registration.sampling_epoch;
  }
  if (publication.sequence.is_zero()) {
    publication.sequence = impl_->next_sequence.next();
  } else if (publication.sequence >= impl_->next_sequence) {
    impl_->next_sequence = publication.sequence.next();
  }

  Frame request;
  request.type = MessageType::PublishCounter;
  request.flags = frame_flags::kAckRequired;
  request.frame_sequence = ++impl_->frame_sequence_;
  PayloadWriter writer;
  const Status written = detail::write_counter_publication(writer, publication);
  if (!written.ok()) {
    outcome.code = written.code();
    outcome.detail = written.describe();
    return Result<CounterOutcome>(std::move(outcome));
  }
  request.payload = writer.data();
  const Result<AckPayload> ack = exchange(impl_->socket_, impl_->buffer_, request);
  if (!ack.ok()) {
    impl_->connected = false;
    outcome.code = ack.code();
    outcome.detail = ack.error().describe();
    return Result<CounterOutcome>(std::move(outcome));
  }
  if (ack.value().status != AckStatus::Accepted) {
    outcome.code = ack_to_status(ack.value()).code();
    outcome.detail = ack.value().detail;
    ++impl_->rejected;
    return Result<CounterOutcome>(std::move(outcome));
  }
  outcome.accepted = true;
  outcome.detail = ack.value().detail;
  return Result<CounterOutcome>(std::move(outcome));
}

Result<BatchOutcome> ObservationPublisher::publish_batch(
    std::vector<Observation> observations) {
  BatchOutcome outcome;
  if (observations.empty()) {
    return Result<BatchOutcome>(std::move(outcome));
  }
  if (observations.size() > Limits::kMaxBatchEvents) {
    return fail_as<BatchOutcome>(ErrorCode::TooLarge, "batch exceeds the maximum event count");
  }
  Frame request;
  PayloadWriter writer;
  writer.u32(static_cast<std::uint32_t>(observations.size()));
  {
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->connected) {
      return fail_as<BatchOutcome>(ErrorCode::ConnectionClosed, "publisher is not connected");
    }
    for (Observation& observation : observations) {
      if (observation.source_publisher.empty()) {
        observation.source_publisher = impl_->registration.id;
      }
      if (observation.publisher_boot.is_zero()) {
        observation.publisher_boot = impl_->registration.boot;
      }
      observation.coordinator_epoch = impl_->registration.coordinator_epoch;
      if (observation.topology_generation.is_zero()) {
        observation.topology_generation = impl_->hello_.topology_generation;
      }
      if (observation.sampling_epoch.is_zero()) {
        observation.sampling_epoch = impl_->registration.sampling_epoch;
      }
      if (observation.evidence_generation.is_zero()) {
        observation.evidence_generation = impl_->registration.evidence_generation;
      }
      if (observation.event_id.is_zero()) {
        observation.event_id = CoherenceEventId{impl_->next_sequence.value()};
      }
      if (observation.sequence.is_zero()) {
        observation.sequence = impl_->next_sequence.next();
      } else if (observation.sequence >= impl_->next_sequence) {
        impl_->next_sequence = observation.sequence.next();
      }
      const Status written = detail::write_observation(writer, observation);
      if (!written.ok()) {
        return fail_as<BatchOutcome>(written.code(), written.describe());
      }
    }
    request.type = MessageType::PublishBatch;
    request.flags = frame_flags::kAckRequired;
    request.frame_sequence = ++impl_->frame_sequence_;
    request.payload = writer.data();
    const Result<AckPayload> ack = exchange(impl_->socket_, impl_->buffer_, request);
    if (!ack.ok()) {
      impl_->connected = false;
      return fail_as<BatchOutcome>(ack.code(), ack.error().describe());
    }
    outcome.accepted = ack.value().accepted;
    outcome.accepted_late = ack.value().late;
    outcome.duplicates = ack.value().duplicates;
    outcome.rejected = ack.value().rejected;
    outcome.missing_sequences = ack.value().missing;
    if (ack.value().status != AckStatus::Accepted &&
        ack.value().status != AckStatus::PartiallyAccepted) {
      return fail_as<BatchOutcome>(ack_to_status(ack.value()).code(), ack.value().detail);
    }
  }
  return Result<BatchOutcome>(std::move(outcome));
}

Status ObservationPublisher::heartbeat() {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->connected) {
    return fail(ErrorCode::ConnectionClosed, "publisher is not connected");
  }
  Frame request;
  request.type = MessageType::Heartbeat;
  request.flags = frame_flags::kAckRequired;
  request.frame_sequence = ++impl_->frame_sequence_;
  const Result<AckPayload> ack = exchange(impl_->socket_, impl_->buffer_, request);
  if (!ack.ok()) {
    impl_->connected = false;
    return Status(ack.error());
  }
  return ack_to_status(ack.value());
}

Status ObservationPublisher::close() {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->socket_.close();
  impl_->connected = false;
  return Status();
}

LossReport ObservationPublisher::loss_report() const {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->loss;
}

struct ObservationClient::Impl : public BaseClient {
  bool connected = false;
  std::mutex mutex;
};

ObservationClient::ObservationClient() : impl_(std::make_unique<Impl>()) {}
ObservationClient::~ObservationClient() { close(); }

Status ObservationClient::connect(const ClientOptions& options) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->connected) {
    return fail(ErrorCode::AlreadyExists, "client is already connected");
  }
  const Status hello = impl_->connect_role(options, ClientRole::Client, options.client_name);
  if (!hello.ok()) {
    return hello;
  }
  impl_->connected = true;
  return Status();
}

bool ObservationClient::connected() const noexcept {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->connected;
}

Result<SnapshotPtr> ObservationClient::query_snapshot(const FindingsOptions& options) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->connected) {
    return fail_as<SnapshotPtr>(ErrorCode::ConnectionClosed, "client is not connected");
  }
  Frame request;
  request.type = MessageType::QuerySnapshot;
  request.flags = frame_flags::kAckRequired;
  request.frame_sequence = ++impl_->frame_sequence_;
  PayloadWriter writer;
  writer.u8(findings_flags(options));
  request.payload = writer.data();
  const Status sent = detail::send_frame(impl_->socket_, request);
  if (!sent.ok()) {
    return Result<SnapshotPtr>(sent.error());
  }
  Result<Frame> response = detail::receive_frame(impl_->socket_, impl_->buffer_);
  if (!response.ok()) {
    return Result<SnapshotPtr>(response.error());
  }
  if (response.value().type != MessageType::SnapshotResponse) {
    return fail_as<SnapshotPtr>(ErrorCode::ProtocolError, "expected SNAPSHOT_RESPONSE");
  }
  PayloadReader reader(response.value().payload.data(), response.value().payload.size());
  Result<SnapshotPtr> snapshot = detail::read_snapshot(reader);
  if (!snapshot.ok()) {
    return snapshot;
  }
  const Status exhausted = reader.require_exhausted();
  if (!exhausted.ok()) {
    return Result<SnapshotPtr>(exhausted.error());
  }
  return snapshot;
}

Result<std::vector<Finding>> ObservationClient::query_findings(
    const FindingsOptions& options) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->connected) {
    return fail_as<std::vector<Finding>>(ErrorCode::ConnectionClosed,
                                         "client is not connected");
  }
  Frame request;
  request.type = MessageType::QueryFindings;
  request.flags = frame_flags::kAckRequired;
  request.frame_sequence = ++impl_->frame_sequence_;
  PayloadWriter writer;
  writer.u8(findings_flags(options));
  request.payload = writer.data();
  const Status sent = detail::send_frame(impl_->socket_, request);
  if (!sent.ok()) {
    return Result<std::vector<Finding>>(sent.error());
  }
  Result<Frame> response = detail::receive_frame(impl_->socket_, impl_->buffer_);
  if (!response.ok()) {
    return Result<std::vector<Finding>>(response.error());
  }
  if (response.value().type != MessageType::FindingsResponse) {
    return fail_as<std::vector<Finding>>(ErrorCode::ProtocolError,
                                         "expected FINDINGS_RESPONSE");
  }
  PayloadReader reader(response.value().payload.data(), response.value().payload.size());
  std::uint32_t count = 0;
  Status status = reader.u32(&count);
  if (!status.ok()) {
    return Result<std::vector<Finding>>(status.error());
  }
  if (count > Limits::kMaxQueryResults) {
    return fail_as<std::vector<Finding>>(ErrorCode::TooMany,
                                         "finding response count exceeds the bound");
  }
  std::vector<Finding> findings;
  for (std::uint32_t i = 0; i < count; ++i) {
    Result<Finding> finding = detail::read_finding(reader);
    if (!finding.ok()) {
      return Result<std::vector<Finding>>(finding.error());
    }
    findings.push_back(finding.take());
  }
  const Status exhausted = reader.require_exhausted();
  if (!exhausted.ok()) {
    return Result<std::vector<Finding>>(exhausted.error());
  }
  return Result<std::vector<Finding>>(std::move(findings));
}

Result<AttributionResult> ObservationClient::query_attribution(
    const MemoryRegionId& region) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->connected) {
    return fail_as<AttributionResult>(ErrorCode::ConnectionClosed, "client is not connected");
  }
  Frame request;
  request.type = MessageType::QueryAttribution;
  request.flags = frame_flags::kAckRequired;
  request.frame_sequence = ++impl_->frame_sequence_;
  PayloadWriter writer;
  Status status = writer.text(region.view(), Limits::kMaxNameLength);
  if (!status.ok()) {
    return Result<AttributionResult>(status.error());
  }
  request.payload = writer.data();
  const Status sent = detail::send_frame(impl_->socket_, request);
  if (!sent.ok()) {
    return Result<AttributionResult>(sent.error());
  }
  Result<Frame> response = detail::receive_frame(impl_->socket_, impl_->buffer_);
  if (!response.ok()) {
    return Result<AttributionResult>(response.error());
  }
  if (response.value().type != MessageType::AttributionResponse) {
    return fail_as<AttributionResult>(ErrorCode::ProtocolError,
                                      "expected ATTRIBUTION_RESPONSE");
  }
  PayloadReader reader(response.value().payload.data(), response.value().payload.size());
  Result<AttributionResult> attribution = detail::read_attribution(reader);
  if (!attribution.ok()) {
    return attribution;
  }
  const Status exhausted = reader.require_exhausted();
  if (!exhausted.ok()) {
    return Result<AttributionResult>(exhausted.error());
  }
  return attribution;
}

Status ObservationClient::close() {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->socket_.close();
  impl_->connected = false;
  return Status();
}

struct ObservatoryAdminClient::Impl : public BaseClient {
  bool connected = false;
  std::mutex mutex;

  Result<AckPayload> send(const Frame& request) {
    return exchange(socket_, buffer_, request);
  }

  Result<AckPayload> simple(MessageType type, const PayloadWriter& writer) {
    if (!connected) {
      return fail_as<AckPayload>(ErrorCode::ConnectionClosed, "admin client is not connected");
    }
    Frame request;
    request.type = type;
    request.flags = frame_flags::kAckRequired;
    request.frame_sequence = ++frame_sequence_;
    request.payload = writer.data();
    return send(request);
  }
};

ObservatoryAdminClient::ObservatoryAdminClient() : impl_(std::make_unique<Impl>()) {}
ObservatoryAdminClient::~ObservatoryAdminClient() { close(); }

Status ObservatoryAdminClient::connect(const ClientOptions& options) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->connected) {
    return fail(ErrorCode::AlreadyExists, "admin client is already connected");
  }
  const Status hello = impl_->connect_role(options, ClientRole::Admin, options.client_name);
  if (!hello.ok()) {
    return hello;
  }
  impl_->connected = true;
  return Status();
}

bool ObservatoryAdminClient::connected() const noexcept {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->connected;
}

namespace {

template <class Fn>
Status admin_request(ObservatoryAdminClient::Impl& impl, MessageType type, Fn&& fill) {
  const std::lock_guard<std::mutex> lock(impl.mutex);
  PayloadWriter writer;
  const Status written = fill(writer);
  if (!written.ok()) {
    return written;
  }
  const Result<AckPayload> ack = impl.simple(type, writer);
  if (!ack.ok()) {
    return Status(ack.error());
  }
  return ack_to_status(ack.value());
}

}  // namespace

Status ObservatoryAdminClient::register_node(const NodeRecord& record) {
  return admin_request(*impl_, MessageType::RegisterResource, [&record](PayloadWriter& w) {
    w.u8(static_cast<std::uint8_t>(ResourceKind::Node));
    return detail::write_node(w, record);
  });
}

Status ObservatoryAdminClient::register_processor(const ProcessorRecord& record) {
  return admin_request(*impl_, MessageType::RegisterResource,
                       [&record](PayloadWriter& w) {
                         w.u8(static_cast<std::uint8_t>(ResourceKind::Processor));
                         return detail::write_processor(w, record);
                       });
}

Status ObservatoryAdminClient::register_accelerator(const AcceleratorRecord& record) {
  return admin_request(*impl_, MessageType::RegisterResource,
                       [&record](PayloadWriter& w) {
                         w.u8(static_cast<std::uint8_t>(ResourceKind::Accelerator));
                         return detail::write_accelerator(w, record);
                       });
}

Status ObservatoryAdminClient::register_memory_domain(const MemoryDomainRecord& record) {
  return admin_request(*impl_, MessageType::RegisterResource,
                       [&record](PayloadWriter& w) {
                         w.u8(static_cast<std::uint8_t>(ResourceKind::MemoryDomain));
                         return detail::write_memory_domain(w, record);
                       });
}

Status ObservatoryAdminClient::register_coherence_domain(
    const CoherenceDomainRecord& record) {
  return admin_request(*impl_, MessageType::RegisterResource,
                       [&record](PayloadWriter& w) {
                         w.u8(static_cast<std::uint8_t>(ResourceKind::CoherenceDomain));
                         return detail::write_coherence_domain(w, record);
                       });
}

Status ObservatoryAdminClient::register_region(const RegionRecord& record) {
  return admin_request(*impl_, MessageType::RegisterRegion, [&record](PayloadWriter& w) {
    return detail::write_region(w, record);
  });
}

Status ObservatoryAdminClient::retire_region(const MemoryRegionId& region,
                                              MemoryRegionGeneration generation,
                                              const std::string& reason) {
  return admin_request(*impl_, MessageType::RetireRegion,
                       [&region, generation, &reason](PayloadWriter& w) {
                         Status status = w.text(region.view(), Limits::kMaxNameLength);
                         if (!status.ok()) return status;
                         w.u64(generation.value());
                         return w.text(reason, Limits::kMaxRegionAnnotationLength);
                       });
}

Status ObservatoryAdminClient::set_topology_link(const TopologyLink& link) {
  return admin_request(*impl_, MessageType::SetTopologyLink, [&link](PayloadWriter& w) {
    return detail::write_topology_link(w, link);
  });
}

Result<TopologyGeneration> ObservatoryAdminClient::bump_topology(const std::string& reason) {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  PayloadWriter writer;
  Status status = writer.text(reason, Limits::kMaxRegionAnnotationLength);
  if (!status.ok()) {
    return Result<TopologyGeneration>(status.error());
  }
  const Result<AckPayload> ack = impl_->simple(MessageType::BumpTopology, writer);
  if (!ack.ok()) {
    return Result<TopologyGeneration>(ack.error());
  }
  const Status accepted = ack_to_status(ack.value());
  if (!accepted.ok()) {
    return Result<TopologyGeneration>(accepted.error());
  }
  return Result<TopologyGeneration>(TopologyGeneration{ack.value().observation_epoch.value()});
}

Status ObservatoryAdminClient::set_capability(const Capability& capability) {
  (void)capability;
  return fail(ErrorCode::UnsupportedCapability,
              "capability mutation is available only on an in-process observatory");
}

Status ObservatoryAdminClient::fence_publisher(const PublisherId& publisher,
                                                FenceReason reason,
                                                const std::string& detail_text) {
  return admin_request(*impl_, MessageType::Fence,
                       [&publisher, reason, &detail_text](PayloadWriter& w) {
                         Status status =
                             w.text(publisher.view(), Limits::kMaxNameLength);
                         if (!status.ok()) return status;
                         w.u8(static_cast<std::uint8_t>(reason));
                         return w.text(detail_text, Limits::kMaxRegionAnnotationLength);
                       });
}

Result<PersistenceReport> ObservatoryAdminClient::save_state() {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  const Result<AckPayload> ack =
      impl_->simple(MessageType::SaveState, PayloadWriter{});
  if (!ack.ok()) {
    return Result<PersistenceReport>(ack.error());
  }
  const Status accepted = ack_to_status(ack.value());
  if (!accepted.ok()) {
    return Result<PersistenceReport>(accepted.error());
  }
  PersistenceReport report;
  report.source_coordinator_epoch = ack.value().coordinator_epoch;
  report.next_coordinator_epoch = ack.value().coordinator_epoch;
  return Result<PersistenceReport>(std::move(report));
}

Status ObservatoryAdminClient::close() {
  const std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->socket_.close();
  impl_->connected = false;
  return Status();
}

}  // namespace sol::coherence
