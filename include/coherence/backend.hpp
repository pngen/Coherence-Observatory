// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Collector interfaces.
//
// A backend is a narrow collector: it discovers structure, and it emits
// observations through the same ingestion path as every other source.  There
// is no test-only alternate pipeline.

#ifndef COHERENCE_BACKEND_HPP
#define COHERENCE_BACKEND_HPP

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "coherence/error.hpp"
#include "coherence/export.hpp"
#include "coherence/observatory.hpp"
#include "coherence/provenance.hpp"
#include "coherence/region.hpp"
#include "coherence/resource.hpp"
#include "coherence/topology.hpp"

namespace sol::coherence {

/// Structural registration available to a collector.
///
/// Remote publishers never receive this interface: only the coordinator or an
/// in-process host may register resources, so an unauthorized publisher cannot
/// invent topology.
class COHERENCE_API StructureRegistrar {
 public:
  virtual ~StructureRegistrar();

  virtual Result<TopologyGeneration> bump_topology(std::string reason) = 0;
  virtual Status register_node(NodeRecord record) = 0;
  virtual Status register_processor(ProcessorRecord record) = 0;
  virtual Status register_accelerator(AcceleratorRecord record) = 0;
  virtual Status register_memory_domain(MemoryDomainRecord record) = 0;
  virtual Status register_coherence_domain(CoherenceDomainRecord record) = 0;
  virtual Status register_region(RegionRecord record) = 0;
  /// Retires a region at an exact generation.  Available only in process; the
  /// coordinator never delegates structural mutation to a remote publisher.
  virtual Status retire_region(const MemoryRegionId& region,
                               MemoryRegionGeneration generation,
                               std::string reason) = 0;
  virtual Status set_topology_link(TopologyLink link) = 0;
  virtual Status set_capability(Capability capability) = 0;
  virtual Status set_cost_model(CostModel model) = 0;
  virtual CoordinatorEpoch coordinator_epoch() const = 0;
  virtual TopologyGeneration topology_generation() const = 0;
};

/// Ingestion target for a collector.
class COHERENCE_API IngestionSink {
 public:
  virtual ~IngestionSink();

  virtual Result<IngestionOutcome> publish(Observation observation) = 0;
  virtual Result<CounterOutcome> publish_counter(CounterPublication publication) = 0;
  /// Publisher identity this sink publishes under.
  virtual PublisherId publisher_id() const = 0;
  virtual PublisherBootId publisher_boot() const = 0;
  virtual CoordinatorEpoch coordinator_epoch() const = 0;
  /// Allocates the next event sequence for this publisher.
  virtual EventSequence next_sequence() = 0;
};

/// Runtime context handed to a collector.
struct COHERENCE_API CollectorContext {
  /// Required.  Observations are published here.
  IngestionSink* sink = nullptr;
  /// Optional.  Null for collectors attached to a remote coordinator, which
  /// may not mutate structural state.
  StructureRegistrar* registrar = nullptr;
  /// Cooperative cancellation flag owned by the runner.
  const std::atomic<bool>* stop_requested = nullptr;
  /// Deterministic seed.
  std::uint64_t seed = 0;
  /// Maximum observations to emit per poll() call.
  std::size_t budget_per_poll = 256;

  bool stopping() const noexcept {
    return stop_requested != nullptr && stop_requested->load(std::memory_order_relaxed);
  }
};

/// A narrow coherence collector.
class COHERENCE_API Collector {
 public:
  virtual ~Collector();

  /// Stable backend name, e.g. "synthetic".
  virtual std::string_view name() const noexcept = 0;
  /// What this backend can and cannot observe on this host.
  virtual std::vector<Capability> capabilities() const = 0;
  /// Discovers structure and prepares to emit.  Must register capabilities.
  virtual Status start(const CollectorContext& context) = 0;
  /// Emits at most context.budget_per_poll observations.  Called repeatedly.
  virtual Status poll(const CollectorContext& context) = 0;
  /// Releases resources.  Must not throw and must be idempotent.
  virtual Status stop() = 0;
};

/// In-process publication host.
///
/// Implements both the ingestion sink a collector publishes into and the
/// structural registrar it registers through, so an in-process collector and a
/// remote publisher take exactly the same validation path.
class COHERENCE_API LocalPublisherHost : public IngestionSink, public StructureRegistrar {
 public:
  struct Options {
    /// Allocate event identities when the observation does not carry one.
    bool auto_event_id = true;
    /// Allocate event sequences when the observation does not carry one.
    bool auto_sequence = true;
    /// Overwrite publisher identity and boot identity with this host's
    /// registration.  Coordinator epoch, evidence generation and sampling
    /// epoch are filled in only when the caller left them unset, so that a
    /// harness can deliberately publish stale-epoch evidence.
    bool stamp_authority = true;
    /// Bind evidence to the topology generation in force at publication time
    /// when the caller did not state one.  A source that produced evidence
    /// under an older topology must state that generation explicitly.
    bool stamp_topology_generation = true;
  };

  LocalPublisherHost(Observatory& observatory, PublisherRegistration registration,
                     Options options = {});
  ~LocalPublisherHost() override;

  LocalPublisherHost(const LocalPublisherHost&) = delete;
  LocalPublisherHost& operator=(const LocalPublisherHost&) = delete;

  /// Registers the publisher with the observatory.
  Status start();

  // ---- IngestionSink ---------------------------------------------------
  Result<IngestionOutcome> publish(Observation observation) override;
  Result<CounterOutcome> publish_counter(CounterPublication publication) override;
  /// Publishes a batch through the same validation path as a single event.
  Result<BatchOutcome> publish_batch(std::vector<Observation> observations);
  PublisherId publisher_id() const override { return registration_.id; }
  PublisherBootId publisher_boot() const override { return registration_.boot; }
  CoordinatorEpoch coordinator_epoch() const override;
  EventSequence next_sequence() override;

  // ---- StructureRegistrar ---------------------------------------------
  Result<TopologyGeneration> bump_topology(std::string reason) override;
  Status register_node(NodeRecord record) override;
  Status register_processor(ProcessorRecord record) override;
  Status register_accelerator(AcceleratorRecord record) override;
  Status register_memory_domain(MemoryDomainRecord record) override;
  Status register_coherence_domain(CoherenceDomainRecord record) override;
  Status register_region(RegionRecord record) override;
  Status retire_region(const MemoryRegionId& region, MemoryRegionGeneration generation,
                       std::string reason) override;
  Status set_topology_link(TopologyLink link) override;
  Status set_capability(Capability capability) override;
  Status set_cost_model(CostModel model) override;
  TopologyGeneration topology_generation() const override;

  const PublisherRegistration& registration() const noexcept { return registration_; }
  std::uint64_t rejected_observations() const noexcept { return rejected_; }
  std::uint64_t accepted_observations() const noexcept { return accepted_; }

 private:
  Observatory* observatory_;
  PublisherRegistration registration_;
  Options options_;
  std::mutex mutex_;
  EventSequence next_sequence_{1};
  CoherenceEventId next_event_id_{1};
  std::uint64_t rejected_ = 0;
  std::uint64_t accepted_ = 0;
};

/// Runs collectors on one bounded worker thread (never one thread per event).
///
/// Collectors are polled round-robin; the thread sleeps for a bounded interval
/// between sweeps and exits promptly when stopped.
class COHERENCE_API CollectorRunner {
 public:
  CollectorRunner();
  ~CollectorRunner();

  CollectorRunner(const CollectorRunner&) = delete;
  CollectorRunner& operator=(const CollectorRunner&) = delete;

  /// Adds a collector.  Bounded by Limits::kMaxThreads collectors.
  Status add(std::shared_ptr<Collector> collector);

  /// Starts the polling thread.  p sink and p registrar are borrowed.
  Status start(IngestionSink* sink, StructureRegistrar* registrar, std::uint64_t seed);
  /// Stops the thread and stops every collector.  Idempotent.
  Status stop();

  bool running() const noexcept;

  /// Number of poll sweeps completed.
  std::uint64_t sweeps() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sol::coherence

#endif  // COHERENCE_BACKEND_HPP
