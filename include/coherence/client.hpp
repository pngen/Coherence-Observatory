// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Remote clients.
//
// ObservationPublisher runs inside a producing process (for example a
// collector host or a workload driver) and publishes evidence under its own
// publisher identity and boot identity.  ObservationClient performs read-only
// queries.  ObservatoryAdminClient performs structural mutation.

#ifndef COHERENCE_CLIENT_HPP
#define COHERENCE_CLIENT_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "coherence/backend.hpp"
#include "coherence/error.hpp"
#include "coherence/export.hpp"
#include "coherence/observatory.hpp"
#include "coherence/snapshot.hpp"

namespace sol::coherence {

/// Connection options shared by all clients.
struct COHERENCE_API ClientOptions {
  std::string host = "127.0.0.1";
  std::uint16_t port = 0;
  /// Visible in the coordinator's publisher listing.
  std::string client_name;
  /// Connect timeout in milliseconds (transport-level only).
  std::uint32_t connect_timeout_ms = 5000;
  /// Receive timeout in milliseconds (transport-level only).
  std::uint32_t receive_timeout_ms = 30000;
  /// Receive buffer bound for responses.
  std::size_t max_response_bytes = Limits::kMaxFramePayload;
};

/// Publishes observations to a coordinator from a producing process.
class COHERENCE_API ObservationPublisher : public IngestionSink {
 public:
  ObservationPublisher();
  ~ObservationPublisher() override;

  ObservationPublisher(const ObservationPublisher&) = delete;
  ObservationPublisher& operator=(const ObservationPublisher&) = delete;

  /// Connects and registers.  Fails if the publisher identity is already
  /// registered under a different boot, or if the boot identity has been
  /// fenced.
  Status connect(const ClientOptions& options, const PublisherRegistration& registration);

  /// True once registered and not yet disconnected.
  bool connected() const noexcept;

  /// Coordinator epoch learned at registration.
  CoordinatorEpoch coordinator_epoch() const override;

  /// Topology generation the coordinator reported at handshake.  A remote
  /// publisher cannot discover topology itself, so it binds its evidence to
  /// the generation the coordinator is actually in.
  TopologyGeneration topology_generation() const;

  PublisherId publisher_id() const override;
  PublisherBootId publisher_boot() const override;
  EventSequence next_sequence() override;

  Result<IngestionOutcome> publish(Observation observation) override;
  Result<CounterOutcome> publish_counter(CounterPublication publication) override;

  /// Publishes a batch in one frame.
  Result<BatchOutcome> publish_batch(std::vector<Observation> observations);

  /// Sends a heartbeat.
  Status heartbeat();

  /// Closes the connection.  Idempotent.
  Status close();

  /// Evidence loss reported by the coordinator for this publisher.
  LossReport loss_report() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Read-only query client.
class COHERENCE_API ObservationClient {
 public:
  ObservationClient();
  ~ObservationClient();

  ObservationClient(const ObservationClient&) = delete;
  ObservationClient& operator=(const ObservationClient&) = delete;

  Status connect(const ClientOptions& options);
  bool connected() const noexcept;

  Result<SnapshotPtr> query_snapshot(const FindingsOptions& options = {});
  Result<std::vector<Finding>> query_findings(const FindingsOptions& options = {});
  Result<AttributionResult> query_attribution(const MemoryRegionId& region);

  Status close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Administrative client: structural mutation only.
class COHERENCE_API ObservatoryAdminClient {
 public:
  ObservatoryAdminClient();
  ~ObservatoryAdminClient();

  ObservatoryAdminClient(const ObservatoryAdminClient&) = delete;
  ObservatoryAdminClient& operator=(const ObservatoryAdminClient&) = delete;

  Status connect(const ClientOptions& options);
  bool connected() const noexcept;

  Status register_node(const NodeRecord& record);
  Status register_processor(const ProcessorRecord& record);
  Status register_accelerator(const AcceleratorRecord& record);
  Status register_memory_domain(const MemoryDomainRecord& record);
  Status register_coherence_domain(const CoherenceDomainRecord& record);
  Status register_region(const RegionRecord& record);
  Status retire_region(const MemoryRegionId& region, MemoryRegionGeneration generation,
                       const std::string& reason);
  Status set_topology_link(const TopologyLink& link);
  Result<TopologyGeneration> bump_topology(const std::string& reason);
  Status set_capability(const Capability& capability);
  Status fence_publisher(const PublisherId& publisher, FenceReason reason,
                         const std::string& detail);
  Result<PersistenceReport> save_state();

  Status close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sol::coherence

#endif  // COHERENCE_CLIENT_HPP
