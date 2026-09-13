// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/backend.hpp"

#include <atomic>
#include "coherence/client.hpp"
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "text_util.hpp"

namespace sol::coherence {

StructureRegistrar::~StructureRegistrar() = default;
IngestionSink::~IngestionSink() = default;
Collector::~Collector() = default;

// ---- LocalPublisherHost ------------------------------------------------

LocalPublisherHost::LocalPublisherHost(Observatory& observatory,
                                       PublisherRegistration registration, Options options)
    : observatory_(&observatory),
      registration_(std::move(registration)),
      options_(options) {}

LocalPublisherHost::~LocalPublisherHost() = default;

Status LocalPublisherHost::start() {
  if (registration_.id.empty()) {
    return fail(ErrorCode::InvalidArgument, "publisher identity is empty");
  }
  if (registration_.boot.is_zero()) {
    registration_.boot = make_publisher_boot_id();
  }
  const Result<PublisherView> view = observatory_->register_publisher(registration_);
  if (!view.ok()) {
    return Status(view.error());
  }
  registration_.coordinator_epoch = view.value().registered_epoch;
  registration_.evidence_generation = view.value().evidence_generation;
  registration_.sampling_epoch = view.value().sampling_epoch;
  return Status();
}

CoordinatorEpoch LocalPublisherHost::coordinator_epoch() const {
  return observatory_->coordinator_epoch();
}

TopologyGeneration LocalPublisherHost::topology_generation() const {
  return observatory_->topology_generation();
}

EventSequence LocalPublisherHost::next_sequence() {
  const std::lock_guard<std::mutex> lock(mutex_);
  const EventSequence current = next_sequence_;
  next_sequence_ = next_sequence_.next();
  return current;
}

Result<IngestionOutcome> LocalPublisherHost::publish(Observation observation) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (options_.stamp_authority) {
      observation.source_publisher = registration_.id;
      observation.publisher_boot = registration_.boot;
    }
    if (observation.coordinator_epoch.is_zero()) {
      observation.coordinator_epoch = observatory_->coordinator_epoch();
    }
    if (options_.stamp_authority) {
      if (observation.sampling_epoch.is_zero()) {
        observation.sampling_epoch = registration_.sampling_epoch;
      }
      if (observation.evidence_generation.is_zero()) {
        observation.evidence_generation = registration_.evidence_generation;
      }
    }
    if (options_.stamp_topology_generation && observation.topology_generation.is_zero()) {
      observation.topology_generation = observatory_->topology_generation();
    }
    if (options_.auto_event_id && observation.event_id.is_zero()) {
      observation.event_id = next_event_id_;
      next_event_id_ = CoherenceEventId{next_event_id_.value() + 1};
    }
    if (options_.auto_sequence) {
      if (observation.sequence.is_zero()) {
        observation.sequence = next_sequence_;
        next_sequence_ = next_sequence_.next();
      } else if (observation.sequence >= next_sequence_) {
        next_sequence_ = observation.sequence.next();
      }
    }
  }
  const Result<IngestionOutcome> outcome = observatory_->ingest(std::move(observation));
  const std::lock_guard<std::mutex> lock(mutex_);
  if (outcome.ok() && outcome.value().disposition != IngestionDisposition::Rejected) {
    ++accepted_;
  } else {
    ++rejected_;
  }
  return outcome;
}

Result<CounterOutcome> LocalPublisherHost::publish_counter(
    CounterPublication publication) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (options_.stamp_authority) {
      publication.publisher = registration_.id;
      publication.publisher_boot = registration_.boot;
    }
    if (publication.coordinator_epoch.is_zero()) {
      publication.coordinator_epoch = observatory_->coordinator_epoch();
    }
    if (options_.stamp_authority) {
      if (publication.sampling_epoch.is_zero()) {
        publication.sampling_epoch = registration_.sampling_epoch;
      }
      if (publication.evidence_generation.is_zero()) {
        publication.evidence_generation = registration_.evidence_generation;
      }
    }
    if (options_.stamp_topology_generation && publication.topology_generation.is_zero()) {
      publication.topology_generation = observatory_->topology_generation();
    }
    if (options_.auto_sequence) {
      if (publication.sequence.is_zero()) {
        publication.sequence = next_sequence_;
        next_sequence_ = next_sequence_.next();
      } else if (publication.sequence >= next_sequence_) {
        next_sequence_ = publication.sequence.next();
      }
    }
  }
  const Result<CounterOutcome> outcome =
      observatory_->ingest_counter(std::move(publication));
  const std::lock_guard<std::mutex> lock(mutex_);
  if (outcome.ok() && outcome.value().accepted) {
    ++accepted_;
  } else {
    ++rejected_;
  }
  return outcome;
}

Result<BatchOutcome> LocalPublisherHost::publish_batch(std::vector<Observation> observations) {
  if (observations.size() > Limits::kMaxBatchEvents) {
    return fail_as<BatchOutcome>(ErrorCode::TooLarge, "batch exceeds the maximum event count");
  }
  return observatory_->ingest_batch(std::move(observations));
}

Result<TopologyGeneration> LocalPublisherHost::bump_topology(std::string reason) {
  return observatory_->bump_topology_generation(std::move(reason));
}

Status LocalPublisherHost::register_node(NodeRecord record) {
  return observatory_->register_node(std::move(record));
}

Status LocalPublisherHost::register_processor(ProcessorRecord record) {
  return observatory_->register_processor(std::move(record));
}

Status LocalPublisherHost::register_accelerator(AcceleratorRecord record) {
  return observatory_->register_accelerator(std::move(record));
}

Status LocalPublisherHost::register_memory_domain(MemoryDomainRecord record) {
  return observatory_->register_memory_domain(std::move(record));
}

Status LocalPublisherHost::register_coherence_domain(CoherenceDomainRecord record) {
  return observatory_->register_coherence_domain(std::move(record));
}

Status LocalPublisherHost::register_region(RegionRecord record) {
  return observatory_->register_region(std::move(record));
}

Status LocalPublisherHost::retire_region(const MemoryRegionId& region,
                                         MemoryRegionGeneration generation,
                                         std::string reason) {
  return observatory_->retire_region(region, generation, std::move(reason));
}

Status LocalPublisherHost::set_topology_link(TopologyLink link) {
  return observatory_->set_topology_link(std::move(link));
}

Status LocalPublisherHost::set_capability(Capability capability) {
  return observatory_->set_capability(std::move(capability));
}

Status LocalPublisherHost::set_cost_model(CostModel model) {
  return observatory_->set_cost_model(std::move(model));
}

// ---- CollectorRunner ---------------------------------------------------

struct CollectorRunner::Impl {
  std::vector<std::shared_ptr<Collector>> collectors;
  std::thread worker;
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> running{false};
  std::atomic<std::uint64_t> sweeps{0};
  std::atomic<std::uint64_t> errors{0};
  CollectorContext context;
  std::uint64_t seed = 0;
};

CollectorRunner::CollectorRunner() : impl_(std::make_unique<Impl>()) {}

CollectorRunner::~CollectorRunner() {
  if (impl_ != nullptr) {
    stop();
  }
}

Status CollectorRunner::add(std::shared_ptr<Collector> collector) {
  if (collector == nullptr) {
    return fail(ErrorCode::InvalidArgument, "collector is null");
  }
  if (impl_->running.load(std::memory_order_relaxed)) {
    return fail(ErrorCode::Busy, "collector runner is already running");
  }
  if (impl_->collectors.size() >= Limits::kMaxThreads) {
    return fail(ErrorCode::TooMany, "collector capacity reached");
  }
  impl_->collectors.push_back(std::move(collector));
  return Status();
}

Status CollectorRunner::start(IngestionSink* sink, StructureRegistrar* registrar,
                              std::uint64_t seed) {
  if (sink == nullptr) {
    return fail(ErrorCode::InvalidArgument, "ingestion sink is required");
  }
  if (impl_->running.load(std::memory_order_relaxed)) {
    return fail(ErrorCode::AlreadyExists, "collector runner is already running");
  }
  impl_->seed = seed;
  impl_->stop_requested.store(false, std::memory_order_relaxed);
  impl_->context.sink = sink;
  impl_->context.registrar = registrar;
  impl_->context.stop_requested = &impl_->stop_requested;
  impl_->context.seed = seed;
  impl_->context.budget_per_poll = 256;
  for (const std::shared_ptr<Collector>& collector : impl_->collectors) {
    const Status started = collector->start(impl_->context);
    if (!started.ok()) {
      return fail(started.code(), "collector failed to start: " + started.describe());
    }
  }
  impl_->running.store(true, std::memory_order_relaxed);
  impl_->worker = std::thread([this]() {
    while (!impl_->stop_requested.load(std::memory_order_relaxed)) {
      for (const std::shared_ptr<Collector>& collector : impl_->collectors) {
        if (impl_->stop_requested.load(std::memory_order_relaxed)) {
          break;
        }
        const Status polled = collector->poll(impl_->context);
        if (!polled.ok()) {
          impl_->errors.fetch_add(1, std::memory_order_relaxed);
        }
      }
      impl_->sweeps.fetch_add(1, std::memory_order_relaxed);
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  return Status();
}

Status CollectorRunner::stop() {
  if (impl_ == nullptr) {
    return Status();
  }
  if (!impl_->running.exchange(false, std::memory_order_relaxed)) {
    return Status();
  }
  impl_->stop_requested.store(true, std::memory_order_relaxed);
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
  for (const std::shared_ptr<Collector>& collector : impl_->collectors) {
    const Status stopped = collector->stop();
    (void)stopped;
  }
  return Status();
}

bool CollectorRunner::running() const noexcept {
  return impl_ != nullptr && impl_->running.load(std::memory_order_relaxed);
}

std::uint64_t CollectorRunner::sweeps() const noexcept {
  if (impl_ == nullptr) {
    return 0;
  }
  return impl_->sweeps.load(std::memory_order_relaxed);
}

}  // namespace sol::coherence
