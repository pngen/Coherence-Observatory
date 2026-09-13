// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Distributed coordinator.
//
// Locking:
//   * sessions_mutex guards the session map only.  It is held briefly for
//     insert, erase and size queries, and is NEVER held while the observatory
//     is touched, while a socket is written, or while a frame is dispatched.
//   * The observatory owns its own state mutex and is never called with
//     sessions_mutex held.
//   * The I/O thread is the only thread that creates or destroys sessions, so
//     it may hold references to session objects without the map lock; other
//     threads read only the guarded size.
//   * stop() sets a flag and joins the I/O thread; it never takes a lock the
//     I/O thread needs in order to observe the flag.

#include "coherence/coordinator.hpp"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "coherence/version.hpp"
#include "detail/platform.hpp"
#include "detail/transport.hpp"
#include "detail/wire.hpp"
#include "text_util.hpp"

namespace sol::coherence {
namespace {

using detail::AckPayload;
using detail::Socket;

constexpr std::uint32_t kPollIntervalMs = 20;

bool role_may_send(ClientRole role, MessageType type) noexcept {
  switch (type) {
    case MessageType::Hello:
    case MessageType::Heartbeat:
      return true;
    case MessageType::RegisterPublisher:
    case MessageType::PublishEvent:
    case MessageType::PublishBatch:
    case MessageType::PublishCounter:
      return role == ClientRole::Publisher || role == ClientRole::Admin;
    case MessageType::QuerySnapshot:
    case MessageType::QueryFindings:
    case MessageType::QueryAttribution:
      return role == ClientRole::Client || role == ClientRole::Admin;
    case MessageType::RegisterResource:
    case MessageType::RegisterRegion:
    case MessageType::RegisterCoherenceDomain:
    case MessageType::SetTopologyLink:
    case MessageType::Fence:
    case MessageType::SaveState:
    case MessageType::BumpTopology:
    case MessageType::RetireRegion:
      return role == ClientRole::Admin;
    case MessageType::HelloAck:
    case MessageType::Ack:
    case MessageType::Error:
    case MessageType::SnapshotResponse:
    case MessageType::FindingsResponse:
    case MessageType::AttributionResponse:
      return false;
  }
  return false;
}

}  // namespace

struct CoordinatorServer::Impl {
  explicit Impl(CoordinatorOptions options_in)
      : options(std::move(options_in)), observatory(options.observatory) {}

  struct Session {
    std::uint64_t id = 0;
    Socket socket;
    ClientRole role = ClientRole::Publisher;
    bool hello_done = false;
    bool publisher_registered = false;
    PublisherId publisher_id;
    PublisherBootId publisher_boot;
    std::string peer;
    std::vector<std::uint8_t> buffer;
  };

  CoordinatorOptions options;
  Observatory observatory;
  Socket listener;
  std::thread io_thread;
  std::atomic<bool> running{false};
  std::atomic<bool> stop_requested{false};
  std::atomic<std::uint64_t> protocol_rejections{0};
  std::atomic<std::uint64_t> connection_count{0};

  mutable std::mutex sessions_mutex;
  std::map<std::uint64_t, Session> sessions;
  std::uint64_t next_session_id = 1;

  std::mutex state_mutex;
  std::string endpoint;

  // ---- helpers ---------------------------------------------------------

  void send_ack(const Session& session, AckStatus status, std::string detail,
                std::uint64_t accepted = 0, std::uint64_t rejected = 0,
                std::uint64_t duplicates = 0, std::uint64_t late = 0,
                std::uint64_t missing = 0) {
    AckPayload ack;
    ack.status = status;
    ack.coordinator_epoch = observatory.coordinator_epoch();
    ack.observation_epoch = observatory.observation_epoch();
    ack.detail = std::move(detail);
    ack.accepted = accepted;
    ack.rejected = rejected;
    ack.duplicates = duplicates;
    ack.late = late;
    ack.missing = missing;
    Frame frame;
    frame.type = status == AckStatus::Accepted || status == AckStatus::PartiallyAccepted
                     ? MessageType::Ack
                     : MessageType::Error;
    frame.flags = frame_flags::kResponse;
    PayloadWriter writer;
    detail::write_ack(writer, ack);
    frame.payload = writer.data();
    detail::send_frame(session.socket, frame);
  }

  void fail_session(Session& session, ErrorCode code, std::string detail) {
    protocol_rejections.fetch_add(1, std::memory_order_relaxed);
    send_ack(session, ack_status_for(code), std::move(detail));
    close_session(session);
  }

  void close_session(Session& session) {
    if (session.publisher_registered) {
      const Result<PublisherView> fenced = observatory.fence_publisher_boot(
          session.publisher_id, session.publisher_boot, FenceReason::ConnectionClosed,
          "publisher transport connection ended (" + session.peer + ")");
      (void)fenced;
      session.publisher_registered = false;
    }
    session.socket.close();
  }

  // ---- dispatch --------------------------------------------------------

  void dispatch(Session& session, const Frame& frame) {
    if (frame.type == MessageType::Hello) {
      handle_hello(session, frame);
      return;
    }
    if (!session.hello_done) {
      fail_session(session, ErrorCode::ProtocolError, "HELLO must precede other messages");
      return;
    }
    if (!role_may_send(session.role, frame.type)) {
      fail_session(session, ErrorCode::Unauthorized,
                   std::string("role ") + std::string(to_string(session.role)) +
                       " may not send " + std::string(to_string(frame.type)));
      return;
    }
    switch (frame.type) {
      case MessageType::Heartbeat: handle_heartbeat(session, frame); break;
      case MessageType::RegisterPublisher: handle_register_publisher(session, frame); break;
      case MessageType::PublishEvent: handle_publish_event(session, frame); break;
      case MessageType::PublishBatch: handle_publish_batch(session, frame); break;
      case MessageType::PublishCounter: handle_publish_counter(session, frame); break;
      case MessageType::QuerySnapshot: handle_query_snapshot(session, frame); break;
      case MessageType::QueryFindings: handle_query_findings(session, frame); break;
      case MessageType::QueryAttribution: handle_query_attribution(session, frame); break;
      case MessageType::RegisterResource: handle_register_resource(session, frame); break;
      case MessageType::RegisterRegion: handle_register_region(session, frame); break;
      case MessageType::RegisterCoherenceDomain:
        handle_register_coherence_domain(session, frame);
        break;
      case MessageType::SetTopologyLink: handle_set_topology_link(session, frame); break;
      case MessageType::Fence: handle_fence(session, frame); break;
      case MessageType::SaveState: handle_save_state(session, frame); break;
      case MessageType::BumpTopology: handle_bump_topology(session, frame); break;
      case MessageType::RetireRegion: handle_retire_region(session, frame); break;
      default:
        fail_session(session, ErrorCode::UnknownMessageType, "unhandled message type");
        break;
    }
  }

  void handle_hello(Session& session, const Frame& frame) {
    PayloadReader reader(frame.payload.data(), frame.payload.size());
    std::uint16_t version = 0;
    std::uint8_t role = 0;
    std::string client_name;
    Status status = reader.u16(&version);
    if (!status.ok()) {
      fail_session(session, status.code(), status.describe());
      return;
    }
    status = reader.u8(&role);
    if (!status.ok()) {
      fail_session(session, status.code(), status.describe());
      return;
    }
    status = reader.text(&client_name, Limits::kMaxNameLength);
    if (!status.ok()) {
      fail_session(session, status.code(), status.describe());
      return;
    }
    status = reader.require_exhausted();
    if (!status.ok()) {
      fail_session(session, status.code(), status.describe());
      return;
    }
    if (version != kWireProtocolVersion) {
      fail_session(session, ErrorCode::UnsupportedVersion, "unsupported protocol version");
      return;
    }
    if (role > static_cast<std::uint8_t>(ClientRole::Admin)) {
      fail_session(session, ErrorCode::InvalidArgument, "unknown client role");
      return;
    }
    if (session.hello_done) {
      fail_session(session, ErrorCode::ProtocolError, "duplicate HELLO");
      return;
    }
    session.role = static_cast<ClientRole>(role);
    session.hello_done = true;

    Frame response;
    response.type = MessageType::HelloAck;
    response.flags = frame_flags::kResponse;
    PayloadWriter writer;
    writer.u16(kWireProtocolVersion);
    writer.u64(observatory.coordinator_epoch().value());
    writer.u64(observatory.observation_epoch().value());
    writer.u64(observatory.topology_generation().value());
    writer.text("coherence-observatory", Limits::kMaxNameLength);
    writer.text(endpoint, Limits::kMaxRegionAnnotationLength);
    writer.u8(static_cast<std::uint8_t>(session.role));
    response.payload = writer.data();
    detail::send_frame(session.socket, response);
  }

  void handle_heartbeat(Session& session, const Frame& frame) {
    if (!frame.payload.empty()) {
      fail_session(session, ErrorCode::TrailingBytes, "HEARTBEAT must have an empty payload");
      return;
    }
    send_ack(session, AckStatus::Accepted, "heartbeat");
  }

  void handle_register_publisher(Session& session, const Frame& frame) {
    PayloadReader reader(frame.payload.data(), frame.payload.size());
    const Result<PublisherRegistration> registration =
        detail::read_publisher_registration(reader);
    if (!registration.ok()) {
      fail_session(session, registration.code(), registration.error().describe());
      return;
    }
    if (!reader.exhausted()) {
      fail_session(session, ErrorCode::TrailingBytes, "trailing bytes in REGISTER_PUBLISHER");
      return;
    }
    const Result<PublisherView> view = observatory.register_publisher(registration.value());
    if (!view.ok()) {
      send_ack(session, ack_status_for(view.code()), view.error().describe());
      return;
    }
    session.publisher_registered = true;
    session.publisher_id = view.value().id;
    session.publisher_boot = view.value().boot;
    send_ack(session, AckStatus::Accepted, "publisher registered");
  }

  void handle_publish_event(Session& session, const Frame& frame) {
    PayloadReader reader(frame.payload.data(), frame.payload.size());
    Result<Observation> observation = detail::read_observation(reader);
    if (!observation.ok()) {
      fail_session(session, observation.code(), observation.error().describe());
      return;
    }
    if (!reader.exhausted()) {
      fail_session(session, ErrorCode::TrailingBytes, "trailing bytes in PUBLISH_EVENT");
      return;
    }
    const Result<IngestionOutcome> outcome = observatory.ingest(observation.take());
    if (!outcome.ok()) {
      send_ack(session, ack_status_for(outcome.code()), outcome.error().describe());
      return;
    }
    AckStatus status = AckStatus::Accepted;
    if (outcome.value().disposition == IngestionDisposition::Duplicate) {
      status = AckStatus::PartiallyAccepted;
    } else if (outcome.value().disposition == IngestionDisposition::Rejected) {
      status = ack_status_for(outcome.value().code);
    }
    send_ack(session, status, outcome.value().detail, 1, 0,
             outcome.value().disposition == IngestionDisposition::Duplicate ? 1 : 0,
             outcome.value().disposition == IngestionDisposition::AcceptedLate ? 1 : 0,
             outcome.value().missing_sequences);
  }

  void handle_publish_batch(Session& session, const Frame& frame) {
    PayloadReader reader(frame.payload.data(), frame.payload.size());
    std::uint32_t count = 0;
    Status status = reader.u32(&count);
    if (!status.ok()) {
      fail_session(session, status.code(), status.describe());
      return;
    }
    if (count == 0 || count > Limits::kMaxBatchEvents) {
      fail_session(session, ErrorCode::TooMany, "batch count out of range");
      return;
    }
    std::vector<Observation> observations;
    observations.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
      Result<Observation> observation = detail::read_observation(reader);
      if (!observation.ok()) {
        fail_session(session, observation.code(), observation.error().describe());
        return;
      }
      observations.push_back(observation.take());
    }
    if (!reader.exhausted()) {
      fail_session(session, ErrorCode::TrailingBytes, "trailing bytes in PUBLISH_BATCH");
      return;
    }
    const Result<BatchOutcome> outcome = observatory.ingest_batch(std::move(observations));
    if (!outcome.ok()) {
      send_ack(session, ack_status_for(outcome.code()), outcome.error().describe());
      return;
    }
    const BatchOutcome& batch = outcome.value();
    send_ack(session, batch.rejected == 0 ? AckStatus::Accepted : AckStatus::PartiallyAccepted,
             "batch", batch.accepted + batch.accepted_late, batch.rejected, batch.duplicates,
             batch.accepted_late, batch.missing_sequences);
  }

  void handle_publish_counter(Session& session, const Frame& frame) {
    PayloadReader reader(frame.payload.data(), frame.payload.size());
    Result<CounterPublication> publication = detail::read_counter_publication(reader);
    if (!publication.ok()) {
      fail_session(session, publication.code(), publication.error().describe());
      return;
    }
    if (!reader.exhausted()) {
      fail_session(session, ErrorCode::TrailingBytes, "trailing bytes in PUBLISH_COUNTER");
      return;
    }
    const Result<CounterOutcome> outcome = observatory.ingest_counter(publication.take());
    if (!outcome.ok()) {
      send_ack(session, ack_status_for(outcome.code()), outcome.error().describe());
      return;
    }
    send_ack(session, AckStatus::Accepted, outcome.value().detail, 1, 0, 0, 0, 0);
  }

  void handle_query_snapshot(Session& session, const Frame& frame) {
    PayloadReader reader(frame.payload.data(), frame.payload.size());
    std::uint8_t flags = 0;
    Status status = reader.u8(&flags);
    if (!status.ok()) {
      fail_session(session, status.code(), status.describe());
      return;
    }
    status = reader.require_exhausted();
    if (!status.ok()) {
      fail_session(session, status.code(), status.describe());
      return;
    }
    FindingsOptions requested;
    requested.compute_hotspots = (flags & 0x01u) != 0;
    requested.compute_ping_pong = (flags & 0x02u) != 0;
    requested.compute_false_sharing = (flags & 0x04u) != 0;
    requested.compute_invalidation = (flags & 0x08u) != 0;
    requested.compute_remote_access = (flags & 0x10u) != 0;
    requested.include_operational = (flags & 0x20u) != 0;
    const SnapshotPtr snapshot = observatory.snapshot(requested);

    PayloadWriter writer;
    const Status serialized = detail::write_snapshot(writer, *snapshot);
    if (!serialized.ok()) {
      // A partially written payload must never reach a client.
      protocol_rejections.fetch_add(1, std::memory_order_relaxed);
      send_ack(session, AckStatus::InternalError,
               "snapshot serialization failed: " + serialized.describe());
      return;
    }
    Frame response;
    response.type = MessageType::SnapshotResponse;
    response.flags = frame_flags::kResponse;
    response.payload = writer.data();
    detail::send_frame(session.socket, response);
  }

  void handle_query_findings(Session& session, const Frame& frame) {
    PayloadReader reader(frame.payload.data(), frame.payload.size());
    std::uint8_t flags = 0;
    Status status = reader.u8(&flags);
    if (!status.ok()) {
      fail_session(session, status.code(), status.describe());
      return;
    }
    status = reader.require_exhausted();
    if (!status.ok()) {
      fail_session(session, status.code(), status.describe());
      return;
    }
    FindingsOptions requested;
    requested.compute_hotspots = (flags & 0x01u) != 0;
    requested.compute_ping_pong = (flags & 0x02u) != 0;
    requested.compute_false_sharing = (flags & 0x04u) != 0;
    requested.compute_invalidation = (flags & 0x08u) != 0;
    requested.compute_remote_access = (flags & 0x10u) != 0;
    requested.include_operational = (flags & 0x20u) != 0;
    const Result<std::vector<Finding>> findings = observatory.findings(requested);
    PayloadWriter writer;
    Status serialized;
    if (!findings.ok()) {
      writer.u32(0);
    } else {
      std::size_t count = findings.value().size();
      if (count > Limits::kMaxQueryResults) {
        count = Limits::kMaxQueryResults;
      }
      writer.u32(static_cast<std::uint32_t>(count));
      for (std::size_t i = 0; i < count && serialized.ok(); ++i) {
        serialized = detail::write_finding(writer, findings.value()[i]);
      }
    }
    if (!serialized.ok()) {
      protocol_rejections.fetch_add(1, std::memory_order_relaxed);
      send_ack(session, AckStatus::InternalError,
               "finding serialization failed: " + serialized.describe());
      return;
    }
    Frame response;
    response.type = MessageType::FindingsResponse;
    response.flags = frame_flags::kResponse;
    response.payload = writer.data();
    detail::send_frame(session.socket, response);
  }

  void handle_query_attribution(Session& session, const Frame& frame) {
    PayloadReader reader(frame.payload.data(), frame.payload.size());
    std::string region;
    Status status = reader.text(&region, Limits::kMaxNameLength);
    if (!status.ok()) {
      fail_session(session, status.code(), status.describe());
      return;
    }
    status = reader.require_exhausted();
    if (!status.ok()) {
      fail_session(session, status.code(), status.describe());
      return;
    }
    const Result<MemoryRegionId> parsed = MemoryRegionId::parse(region);
    if (!parsed.ok()) {
      send_ack(session, AckStatus::Malformed, "invalid region identity");
      return;
    }
    const Result<AttributionResult> attribution = observatory.attribute_region(parsed.value());
    Frame response;
    response.type = MessageType::AttributionResponse;
    response.flags = frame_flags::kResponse;
    PayloadWriter writer;
    if (!attribution.ok()) {
      AttributionResult empty;
      detail::write_attribution(writer, empty);
    } else {
      detail::write_attribution(writer, attribution.value());
    }
    response.payload = writer.data();
    detail::send_frame(session.socket, response);
  }

  void handle_register_resource(Session& session, const Frame& frame) {
    PayloadReader reader(frame.payload.data(), frame.payload.size());
    std::uint8_t kind = 0;
    Status status = reader.u8(&kind);
    if (!status.ok()) {
      fail_session(session, status.code(), status.describe());
      return;
    }
    if (kind > static_cast<std::uint8_t>(ResourceKind::CoherenceDomain)) {
      fail_session(session, ErrorCode::InvalidArgument, "resource kind out of range");
      return;
    }
    Status registration;
    switch (static_cast<ResourceKind>(kind)) {
      case ResourceKind::Node: {
        const Result<NodeRecord> record = detail::read_node(reader);
        if (!record.ok()) { fail_session(session, record.code(), record.error().describe()); return; }
        registration = observatory.register_node(record.value());
        break;
      }
      case ResourceKind::Processor: {
        const Result<ProcessorRecord> record = detail::read_processor(reader);
        if (!record.ok()) { fail_session(session, record.code(), record.error().describe()); return; }
        registration = observatory.register_processor(record.value());
        break;
      }
      case ResourceKind::Accelerator: {
        const Result<AcceleratorRecord> record = detail::read_accelerator(reader);
        if (!record.ok()) { fail_session(session, record.code(), record.error().describe()); return; }
        registration = observatory.register_accelerator(record.value());
        break;
      }
      case ResourceKind::MemoryDomain: {
        const Result<MemoryDomainRecord> record = detail::read_memory_domain(reader);
        if (!record.ok()) { fail_session(session, record.code(), record.error().describe()); return; }
        registration = observatory.register_memory_domain(record.value());
        break;
      }
      case ResourceKind::CoherenceDomain: {
        const Result<CoherenceDomainRecord> record = detail::read_coherence_domain(reader);
        if (!record.ok()) { fail_session(session, record.code(), record.error().describe()); return; }
        registration = observatory.register_coherence_domain(record.value());
        break;
      }
      case ResourceKind::Unknown:
        fail_session(session, ErrorCode::InvalidArgument, "unknown resource kind");
        return;
    }
    if (!reader.exhausted()) {
      fail_session(session, ErrorCode::TrailingBytes, "trailing bytes in REGISTER_RESOURCE");
      return;
    }
    if (!registration.ok()) {
      send_ack(session, ack_status_for(registration.code()), registration.describe());
      return;
    }
    send_ack(session, AckStatus::Accepted, "resource registered");
  }

  void handle_register_region(Session& session, const Frame& frame) {
    PayloadReader reader(frame.payload.data(), frame.payload.size());
    const Result<RegionRecord> record = detail::read_region(reader);
    if (!record.ok()) {
      fail_session(session, record.code(), record.error().describe());
      return;
    }
    if (!reader.exhausted()) {
      fail_session(session, ErrorCode::TrailingBytes, "trailing bytes in REGISTER_REGION");
      return;
    }
    const Status registration = observatory.register_region(record.value());
    if (!registration.ok()) {
      send_ack(session, ack_status_for(registration.code()), registration.describe());
      return;
    }
    send_ack(session, AckStatus::Accepted, "region registered");
  }

  void handle_register_coherence_domain(Session& session, const Frame& frame) {
    PayloadReader reader(frame.payload.data(), frame.payload.size());
    const Result<CoherenceDomainRecord> record = detail::read_coherence_domain(reader);
    if (!record.ok()) {
      fail_session(session, record.code(), record.error().describe());
      return;
    }
    if (!reader.exhausted()) {
      fail_session(session, ErrorCode::TrailingBytes,
                   "trailing bytes in REGISTER_COHERENCE_DOMAIN");
      return;
    }
    const Status registration = observatory.register_coherence_domain(record.value());
    if (!registration.ok()) {
      send_ack(session, ack_status_for(registration.code()), registration.describe());
      return;
    }
    send_ack(session, AckStatus::Accepted, "coherence domain registered");
  }

  void handle_set_topology_link(Session& session, const Frame& frame) {
    PayloadReader reader(frame.payload.data(), frame.payload.size());
    const Result<TopologyLink> link = detail::read_topology_link(reader);
    if (!link.ok()) {
      fail_session(session, link.code(), link.error().describe());
      return;
    }
    if (!reader.exhausted()) {
      fail_session(session, ErrorCode::TrailingBytes, "trailing bytes in SET_TOPOLOGY_LINK");
      return;
    }
    const Status stored = observatory.set_topology_link(link.value());
    if (!stored.ok()) {
      send_ack(session, ack_status_for(stored.code()), stored.describe());
      return;
    }
    send_ack(session, AckStatus::Accepted, "topology link stored");
  }

  void handle_fence(Session& session, const Frame& frame) {
    PayloadReader reader(frame.payload.data(), frame.payload.size());
    std::string publisher;
    std::uint8_t reason = 0;
    std::string detail_text;
    Status status = reader.text(&publisher, Limits::kMaxNameLength);
    if (!status.ok()) { fail_session(session, status.code(), status.describe()); return; }
    status = reader.u8(&reason);
    if (!status.ok()) { fail_session(session, status.code(), status.describe()); return; }
    status = reader.text(&detail_text, Limits::kMaxRegionAnnotationLength);
    if (!status.ok()) { fail_session(session, status.code(), status.describe()); return; }
    status = reader.require_exhausted();
    if (!status.ok()) { fail_session(session, status.code(), status.describe()); return; }
    if (reason > static_cast<std::uint8_t>(FenceReason::SupersededByNewBoot)) {
      fail_session(session, ErrorCode::InvalidArgument, "fence reason out of range");
      return;
    }
    const Result<PublisherId> parsed = PublisherId::parse(publisher);
    if (!parsed.ok()) {
      send_ack(session, AckStatus::Malformed, "invalid publisher identity");
      return;
    }
    const Result<PublisherView> fenced = observatory.fence_publisher(
        parsed.value(), static_cast<FenceReason>(reason), detail_text);
    if (!fenced.ok()) {
      send_ack(session, ack_status_for(fenced.code()), fenced.error().describe());
      return;
    }
    send_ack(session, AckStatus::Accepted, "publisher fenced");
  }

  void handle_bump_topology(Session& session, const Frame& frame) {
    PayloadReader reader(frame.payload.data(), frame.payload.size());
    std::string reason;
    Status status = reader.text(&reason, Limits::kMaxRegionAnnotationLength);
    if (!status.ok()) { fail_session(session, status.code(), status.describe()); return; }
    status = reader.require_exhausted();
    if (!status.ok()) { fail_session(session, status.code(), status.describe()); return; }
    const Result<TopologyGeneration> generation = observatory.bump_topology_generation(reason);
    if (!generation.ok()) {
      send_ack(session, ack_status_for(generation.code()), generation.error().describe());
      return;
    }
    send_ack(session, AckStatus::Accepted, "topology generation advanced");
  }

  void handle_retire_region(Session& session, const Frame& frame) {
    PayloadReader reader(frame.payload.data(), frame.payload.size());
    std::string region;
    std::uint64_t generation = 0;
    std::string reason;
    Status status = reader.text(&region, Limits::kMaxNameLength);
    if (!status.ok()) { fail_session(session, status.code(), status.describe()); return; }
    status = reader.u64(&generation);
    if (!status.ok()) { fail_session(session, status.code(), status.describe()); return; }
    status = reader.text(&reason, Limits::kMaxRegionAnnotationLength);
    if (!status.ok()) { fail_session(session, status.code(), status.describe()); return; }
    status = reader.require_exhausted();
    if (!status.ok()) { fail_session(session, status.code(), status.describe()); return; }
    const Result<MemoryRegionId> parsed = MemoryRegionId::parse(region);
    if (!parsed.ok()) {
      send_ack(session, AckStatus::Malformed, "invalid region identity");
      return;
    }
    const Status retired = observatory.retire_region(parsed.value(),
                                                     MemoryRegionGeneration{generation}, reason);
    if (!retired.ok()) {
      send_ack(session, ack_status_for(retired.code()), retired.describe());
      return;
    }
    send_ack(session, AckStatus::Accepted, "region retired");
  }

  void handle_save_state(Session& session, const Frame& frame) {
    if (!frame.payload.empty()) {
      fail_session(session, ErrorCode::TrailingBytes, "SAVE_STATE must have an empty payload");
      return;
    }
    if (options.state_path.empty()) {
      send_ack(session, AckStatus::Unsupported, "no durable state path is configured");
      return;
    }
    const std::lock_guard<std::mutex> lock(state_mutex);
    const Result<PersistenceReport> report =
        observatory.save_state(options.state_path, options.persistence);
    if (!report.ok()) {
      send_ack(session, ack_status_for(report.code()), report.error().describe());
      return;
    }
    send_ack(session, AckStatus::Accepted,
             "state saved (" + detail::format_u64(report.value().bytes_written) + " bytes)");
  }

  // ---- I/O loop --------------------------------------------------------

  Status accept_connection() {
    Result<Socket> accepted = detail::tcp_accept(listener);
    if (!accepted.ok()) {
      return Status(accepted.error());
    }
    std::size_t count = 0;
    {
      const std::lock_guard<std::mutex> lock(sessions_mutex);
      count = sessions.size();
    }
    if (count >= options.max_connections) {
      accepted.value().close();
      return fail(ErrorCode::Capacity, "connection capacity reached");
    }
    Session session;
    session.id = next_session_id++;
    session.socket = accepted.take();
    const Result<std::string> peer = detail::socket_peer_text(session.socket);
    session.peer = peer.ok() ? peer.value() : std::string("unknown");
    const Status timeouts = detail::socket_set_timeouts(session.socket, 0, 30000);
    (void)timeouts;
    {
      const std::lock_guard<std::mutex> lock(sessions_mutex);
      sessions.emplace(session.id, std::move(session));
    }
    return Status();
  }

  void service_session(std::uint64_t id) {
    Session* session = nullptr;
    {
      const std::lock_guard<std::mutex> lock(sessions_mutex);
      const auto it = sessions.find(id);
      if (it == sessions.end()) {
        return;
      }
      session = &it->second;
    }
    // Only the I/O thread creates or destroys sessions, so this reference is
    // stable for the duration of the call.
    std::array<std::uint8_t, 16384> chunk{};
    const Result<std::size_t> received =
        detail::socket_recv_some(session->socket, chunk.data(), chunk.size());
    if (!received.ok()) {
      if (received.code() == ErrorCode::Busy) {
        return;
      }
      close_session(*session);
      erase_session(id);
      return;
    }
    if (received.value() == 0) {
      close_session(*session);
      erase_session(id);
      return;
    }
    session->buffer.insert(session->buffer.end(), chunk.begin(),
                           chunk.begin() + static_cast<std::ptrdiff_t>(received.value()));
    if (session->buffer.size() > Limits::kMaxFramePayload + Limits::kFrameHeaderSize +
                                     Limits::kFrameTrailerSize) {
      fail_session(*session, ErrorCode::TooLarge, "receive buffer exceeds the frame bound");
      erase_session(id);
      return;
    }

    while (true) {
      if (session->buffer.size() < Limits::kFrameHeaderSize) {
        return;
      }
      FrameHeader header;
      const Status decoded =
          FrameHeader::decode(session->buffer.data(), session->buffer.size(), &header);
      if (!decoded.ok()) {
        fail_session(*session, decoded.code(), decoded.describe());
        erase_session(id);
        return;
      }
      const std::size_t total =
          Limits::kFrameHeaderSize + header.payload_length + Limits::kFrameTrailerSize;
      if (session->buffer.size() < total) {
        return;
      }
      Result<Frame> frame = Frame::decode(session->buffer.data(), total);
      if (!frame.ok()) {
        fail_session(*session, frame.code(), frame.error().describe());
        erase_session(id);
        return;
      }
      session->buffer.erase(session->buffer.begin(),
                            session->buffer.begin() + static_cast<std::ptrdiff_t>(total));
      const bool closed = !session->socket.valid();
      dispatch(*session, frame.value());
      if (!session->socket.valid() || closed) {
        erase_session(id);
        return;
      }
    }
  }

  void erase_session(std::uint64_t id) {
    const std::lock_guard<std::mutex> lock(sessions_mutex);
    sessions.erase(id);
  }

  void io_loop() {
    while (!stop_requested.load(std::memory_order_relaxed)) {
      std::vector<std::uint64_t> ready;
      const Result<bool> listener_ready = detail::socket_is_readable(listener, 0);
      if (listener_ready.ok() && listener_ready.value()) {
        accept_connection();
      }
      {
        const std::lock_guard<std::mutex> lock(sessions_mutex);
        for (const auto& entry : sessions) {
          ready.push_back(entry.first);
        }
      }
      for (std::uint64_t id : ready) {
        bool readable = false;
        {
          const std::lock_guard<std::mutex> lock(sessions_mutex);
          const auto it = sessions.find(id);
          if (it == sessions.end()) {
            continue;
          }
          const Result<bool> probe = detail::socket_is_readable(it->second.socket, 0);
          readable = probe.ok() && probe.value();
        }
        if (readable) {
          service_session(id);
        }
      }
      {
        const std::lock_guard<std::mutex> lock(sessions_mutex);
        connection_count.store(sessions.size(), std::memory_order_relaxed);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }
    // Close every remaining session and fence their publishers.
    std::vector<std::uint64_t> remaining;
    {
      const std::lock_guard<std::mutex> lock(sessions_mutex);
      for (const auto& entry : sessions) {
        remaining.push_back(entry.first);
      }
    }
    for (std::uint64_t id : remaining) {
      Session* session = nullptr;
      {
        const std::lock_guard<std::mutex> lock(sessions_mutex);
        const auto it = sessions.find(id);
        if (it == sessions.end()) {
          continue;
        }
        session = &it->second;
      }
      close_session(*session);
      erase_session(id);
    }
    connection_count.store(0, std::memory_order_relaxed);
  }
};

CoordinatorServer::CoordinatorServer(CoordinatorOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

CoordinatorServer::~CoordinatorServer() { stop(); }

Status CoordinatorServer::start() {
  if (impl_->running.load(std::memory_order_relaxed)) {
    return fail(ErrorCode::AlreadyExists, "coordinator is already running");
  }
  const Status initialized = detail::network_init();
  if (!initialized.ok()) {
    return initialized;
  }
  if (impl_->options.load_on_start && !impl_->options.state_path.empty()) {
    const Result<PersistenceReport> loaded = impl_->observatory.load_state(
        impl_->options.state_path, impl_->options.persistence);
    if (!loaded.ok() && loaded.code() != ErrorCode::NotFound) {
      return Status(loaded.error());
    }
  }
  Result<Socket> listener =
      detail::tcp_listen(impl_->options.bind_host, impl_->options.port,
                         impl_->options.max_connections);
  if (!listener.ok()) {
    return Status(listener.error());
  }
  const Result<std::uint16_t> port = detail::socket_local_port(listener.value());
  if (!port.ok()) {
    return Status(port.error());
  }
  impl_->listener = listener.take();
  impl_->endpoint = impl_->options.bind_host + ":" + detail::format_u64(port.value());
  impl_->stop_requested.store(false, std::memory_order_relaxed);
  impl_->running.store(true, std::memory_order_relaxed);
  impl_->io_thread = std::thread([this]() { impl_->io_loop(); });
  return Status();
}

Status CoordinatorServer::stop() {
  if (!impl_) {
    return Status();
  }
  if (!impl_->running.exchange(false, std::memory_order_relaxed)) {
    return Status();
  }
  impl_->stop_requested.store(true, std::memory_order_relaxed);
  if (impl_->io_thread.joinable()) {
    impl_->io_thread.join();
  }
  impl_->listener.close();
  if (impl_->options.save_on_shutdown && !impl_->options.state_path.empty()) {
    const std::lock_guard<std::mutex> lock(impl_->state_mutex);
    const Result<PersistenceReport> saved = impl_->observatory.save_state(
        impl_->options.state_path, impl_->options.persistence);
    if (!saved.ok()) {
      return Status(saved.error());
    }
  }
  return Status();
}

bool CoordinatorServer::running() const noexcept {
  return impl_ != nullptr && impl_->running.load(std::memory_order_relaxed);
}

std::uint16_t CoordinatorServer::port() const noexcept {
  if (impl_ == nullptr) {
    return 0;
  }
  const Result<std::uint16_t> port = detail::socket_local_port(impl_->listener);
  return port.ok() ? port.value() : 0;
}

std::string CoordinatorServer::endpoint() const {
  if (impl_ == nullptr) {
    return std::string();
  }
  return impl_->endpoint;
}

Observatory& CoordinatorServer::observatory() noexcept { return impl_->observatory; }

const Observatory& CoordinatorServer::observatory() const noexcept {
  return impl_->observatory;
}

std::size_t CoordinatorServer::connection_count() const noexcept {
  if (impl_ == nullptr) {
    return 0;
  }
  return static_cast<std::size_t>(
      impl_->connection_count.load(std::memory_order_relaxed));
}

std::uint64_t CoordinatorServer::protocol_rejections() const noexcept {
  if (impl_ == nullptr) {
    return 0;
  }
  return impl_->protocol_rejections.load(std::memory_order_relaxed);
}

}  // namespace sol::coherence
