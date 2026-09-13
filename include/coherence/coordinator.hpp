// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Distributed coordinator.
//
// The coordinator owns the authoritative observatory state and serves real
// framed transport connections from independent publisher processes.  A
// publisher that dies loses authority for its boot identity the moment its
// connection ends; nothing about that publisher becomes current again except a
// fresh registration under a fresh boot identity.

#ifndef COHERENCE_COORDINATOR_HPP
#define COHERENCE_COORDINATOR_HPP

#include <cstdint>
#include <memory>
#include <string>

#include "coherence/error.hpp"
#include "coherence/export.hpp"
#include "coherence/observatory.hpp"
#include "coherence/protocol.hpp"

namespace sol::coherence {

/// Coordinator configuration.
struct COHERENCE_API CoordinatorOptions {
  /// Interface to bind.  "127.0.0.1" keeps the coordinator local-only.
  std::string bind_host = "127.0.0.1";
  /// Port; 0 selects an ephemeral port (read it back with port()).
  std::uint16_t port = 0;
  std::size_t max_connections = Limits::kMaxConnections;
  /// Durable state file.  Empty disables persistence.
  std::string state_path;
  /// Save durable state automatically on clean shutdown.
  bool save_on_shutdown = true;
  /// Load durable state and perform recovery at start.
  bool load_on_start = true;
  ObservatoryOptions observatory;
  PersistenceOptions persistence;
  /// Grace period between losing a publisher connection and declaring the boot
  /// fenced.  Zero fences immediately on connection close.
  std::int64_t publisher_reap_delay_ns = 0;
};

/// Listens, accepts, validates and dispatches framed protocol traffic.
class COHERENCE_API CoordinatorServer {
 public:
  explicit CoordinatorServer(CoordinatorOptions options = {});
  ~CoordinatorServer();

  CoordinatorServer(const CoordinatorServer&) = delete;
  CoordinatorServer& operator=(const CoordinatorServer&) = delete;

  /// Binds, listens and starts the I/O loop.
  Status start();
  /// Stops accepting, closes connections, joins the I/O thread and (when
  /// configured) saves durable state.  Idempotent; leaves no orphan threads.
  Status stop();

  bool running() const noexcept;
  std::uint16_t port() const noexcept;
  std::string endpoint() const;

  /// Live observatory state.  Safe for concurrent use.
  Observatory& observatory() noexcept;
  const Observatory& observatory() const noexcept;

  /// Number of connections currently accepted.
  std::size_t connection_count() const noexcept;
  /// Frames rejected by protocol-level validation since start.
  std::uint64_t protocol_rejections() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sol::coherence

#endif  // COHERENCE_COORDINATOR_HPP
