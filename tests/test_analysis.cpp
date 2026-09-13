// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "test_framework.hpp"
#include "test_support.hpp"

#include "coherence/explanation.hpp"
#include "coherence/observatory.hpp"

namespace {

using namespace sol::coherence;

bool has_reason(const Finding& finding, std::string_view reason) {
  for (const std::string& existing : finding.reasons) {
    if (existing == reason) {
      return true;
    }
  }
  return false;
}

void publish_ownership_alternation(sol::coherence::LocalPublisherHost& host, const char* region,
                                   int count, Nanos spacing, std::size_t region_generation = 1) {
  const char* participants[2] = {"acc.a", "acc.b"};
  Nanos clock = monotonic_now_ns();
  for (int i = 0; i < count; ++i) {
    Observation observation;
    observation.type = EventType::OwnershipTransfer;
    observation.timestamp_ns = clock;
    clock += spacing;
    observation.source = cotest::ref_accelerator(participants[i % 2]);
    observation.target = cotest::ref_accelerator(participants[(i + 1) % 2]);
    observation.region = MemoryRegionId{region};
    observation.region_generation = MemoryRegionGeneration{region_generation};
    observation.state_before = CoherenceState::Modified;
    observation.state_after = CoherenceState::Invalid;
    observation.precision = Precision::ExactEvent;
    observation.granularity = EvidenceGranularity::Region;
    observation.provenance = Provenance::SyntheticBackend;
    host.publish(std::move(observation));
  }
}

CO_TEST(ping_pong_is_detected_from_exact_transfers) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  publish_ownership_alternation(*host, "region.test.1", 12, 20000);

  const Result<std::vector<Finding>> findings = observatory.ping_pong();
  CO_REQUIRE(findings.ok());
  CO_REQUIRE(!findings.value().empty());
  const Finding& finding = findings.value().front();
  CO_CHECK(finding.kind == FindingKind::PingPong);
  CO_CHECK(finding.subject == "region.test.1");
  CO_CHECK(finding.alternations >= 4);
  CO_CHECK_EQ(finding.participants.size(), std::size_t{2});
  CO_CHECK(!finding.direction_sequence.empty());
  CO_CHECK(!finding.evidence.empty());
  CO_CHECK(finding.precision == Precision::ExactEvent);
  CO_CHECK(has_reason(finding, finding_reason::kAlternatingOwnership));
  CO_CHECK(finding.cost.ownership_transfers >= 12);
}

CO_TEST(ping_pong_requires_evidence_precision) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  const char* participants[2] = {"acc.a", "acc.b"};
  Nanos clock = monotonic_now_ns();
  for (int i = 0; i < 12; ++i) {
    Observation observation;
    observation.type = EventType::OwnershipTransfer;
    observation.timestamp_ns = clock;
    clock += 20000;
    observation.source = cotest::ref_accelerator(participants[i % 2]);
    observation.target = cotest::ref_accelerator(participants[(i + 1) % 2]);
    observation.region = MemoryRegionId{"region.test.1"};
    observation.region_generation = MemoryRegionGeneration{1};
    observation.precision = Precision::AggregatedCounter;
    observation.granularity = EvidenceGranularity::Device;
    observation.provenance = Provenance::SyntheticBackend;
    host->publish(std::move(observation));
  }
  const Result<std::vector<Finding>> findings = observatory.ping_pong();
  CO_REQUIRE(findings.ok());
  CO_CHECK(findings.value().empty());
}

CO_TEST(ping_pong_ignores_a_long_gap) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  publish_ownership_alternation(*host, "region.test.1", 3, 20000);
  publish_ownership_alternation(*host, "region.test.1", 3, 500000000);
  const Result<std::vector<Finding>> findings = observatory.ping_pong();
  CO_REQUIRE(findings.ok());
  CO_CHECK(findings.value().empty());
}

CO_TEST(hotspot_findings_are_decomposed) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  Nanos clock = monotonic_now_ns();
  for (int i = 0; i < 200; ++i) {
    Observation observation = cotest::exact_observation(0, EventType::RemoteRead, clock);
    clock += 1000;
    observation.bytes = 4096;
    observation.pages = 1;
    host->publish(std::move(observation));
  }
  const Result<std::vector<Finding>> findings = observatory.hotspots();
  CO_REQUIRE(findings.ok());
  CO_REQUIRE(!findings.value().empty());
  bool saw_remote_read_reason = false;
  for (const Finding& finding : findings.value()) {
    CO_CHECK(!finding.reasons.empty());
    CO_CHECK(!finding.metrics.empty());
    if (has_reason(finding, finding_reason::kHighRemoteReadRate)) {
      saw_remote_read_reason = true;
      bool threshold_present = false;
      for (const FindingMetric& metric : finding.metrics) {
        if (metric.has_threshold) {
          threshold_present = true;
        }
      }
      CO_CHECK(threshold_present);
    }
  }
  CO_CHECK(saw_remote_read_reason);
}

CO_TEST(false_sharing_requires_line_identity) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  Nanos clock = monotonic_now_ns();
  for (int i = 0; i < 8; ++i) {
    Observation write = cotest::exact_observation(2, EventType::WriteExclusiveTransition, clock);
    clock += 1000;
    write.direction = AccessDirection::Write;
    write.granularity = EvidenceGranularity::CacheLine;
    host->publish(write);
    Observation invalidation = cotest::exact_observation(2, EventType::Invalidation, clock);
    clock += 1000;
    invalidation.granularity = EvidenceGranularity::CacheLine;
    host->publish(invalidation);
  }
  const Result<std::vector<Finding>> findings = observatory.false_sharing();
  CO_REQUIRE(findings.ok());
  CO_REQUIRE(!findings.value().empty());
  const Finding& finding = findings.value().front();
  CO_CHECK(finding.contention == ContentionClass::InsufficientGranularity);
  CO_CHECK(!finding.missing_evidence.empty());
}

CO_TEST(false_sharing_supported_with_line_identity) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  Nanos clock = monotonic_now_ns();
  for (int i = 0; i < 8; ++i) {
    Observation write = cotest::exact_observation(2, EventType::WriteExclusiveTransition, clock);
    clock += 1000;
    write.direction = AccessDirection::Write;
    write.granularity = EvidenceGranularity::CacheLine;
    write.metadata.add("line.index", "4");
    host->publish(write);
    Observation other = cotest::exact_observation(2, EventType::WriteExclusiveTransition, clock);
    clock += 1000;
    other.direction = AccessDirection::Write;
    other.granularity = EvidenceGranularity::CacheLine;
    other.source = cotest::ref_accelerator("acc.b");
    other.metadata.add("line.index", "4");
    host->publish(other);
    Observation invalidation = cotest::exact_observation(2, EventType::Invalidation, clock);
    clock += 1000;
    invalidation.granularity = EvidenceGranularity::CacheLine;
    invalidation.metadata.add("line.index", "4");
    host->publish(invalidation);
  }
  const Result<std::vector<Finding>> findings = observatory.false_sharing();
  CO_REQUIRE(findings.ok());
  CO_REQUIRE(!findings.value().empty());
  CO_CHECK(findings.value().front().contention ==
           ContentionClass::CacheLineFalseSharingSupported);
}

CO_TEST(page_granularity_is_classified_as_such) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  Nanos clock = monotonic_now_ns();
  for (int i = 0; i < 8; ++i) {
    Observation write = cotest::exact_observation(0, EventType::WriteExclusiveTransition, clock);
    clock += 1000;
    write.direction = AccessDirection::Write;
    write.granularity = EvidenceGranularity::Page;
    write.precision = Precision::SampledEvent;
    write.pages = 1;
    host->publish(write);
  }
  // Page-level evidence is sampled, so the policy must accept sampled
  // precision; the classification still reports the page granularity.
  FalseSharingPolicy policy;
  policy.min_precision = Precision::SampledEvent;
  const Result<std::vector<Finding>> findings = observatory.false_sharing(policy);
  CO_REQUIRE(findings.ok());
  CO_REQUIRE(!findings.value().empty());
  CO_CHECK(findings.value().front().contention == ContentionClass::PageLevelContention);
  CO_CHECK(findings.value().front().granularity == EvidenceGranularity::Page);
}

CO_TEST(invalidation_bursts_are_detected_and_aggregate_counters_are_separated) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  Nanos clock = monotonic_now_ns();
  for (int i = 0; i < 16; ++i) {
    Observation observation = cotest::exact_observation(0, EventType::Invalidation, clock);
    clock += 1000;
    observation.source = cotest::ref_accelerator("acc.a");
    observation.target = cotest::ref_accelerator("acc.b");
    observation.state_before = CoherenceState::Shared;
    observation.state_after = CoherenceState::Invalid;
    host->publish(std::move(observation));
  }
  // An aggregate invalidation counter with no source/target identity.
  CounterPublication publication;
  publication.counter = CounterId{"ctr.invalidations"};
  publication.mapped_type = EventType::Invalidation;
  publication.kind = CounterKind::Delta;
  publication.scope = CounterScope::PerDevice;
  publication.width_bits = 64;
  publication.generation = CounterGeneration{1};
  publication.sampling_epoch = SamplingEpoch{1};
  publication.raw_value = 4096;
  publication.provenance = Provenance::HardwarePerformanceCounter;
  publication.granularity = EvidenceGranularity::Device;
  publication.source = cotest::ref_accelerator("acc.a");
  publication.timestamp_ns = clock;
  host->publish_counter(publication);

  const Result<std::vector<InvalidationRow>> rows = observatory.invalidation_analysis();
  CO_REQUIRE(rows.ok());
  CO_REQUIRE(!rows.value().empty());
  std::uint64_t individual = 0;
  for (const InvalidationRow& row : rows.value()) {
    individual += row.individual_events;
    CO_CHECK(row.max_burst_size > 0);
  }
  CO_CHECK_EQ(individual, std::uint64_t{16});
  for (const InvalidationRow& row : rows.value()) {
    CO_CHECK(row.precision != Precision::ExactEvent || row.source != "unknown");
  }

  const Result<std::vector<Finding>> findings = observatory.findings();
  CO_REQUIRE(findings.ok());
  bool separated = false;
  for (const Finding& finding : findings.value()) {
    if (finding.kind != FindingKind::InvalidationBurst) {
      continue;
    }
    if (finding.subject == "aggregate-invalidation-counters") {
      separated = true;
      CO_CHECK(has_reason(finding, finding_reason::kInsufficientGranularity));
      CO_CHECK(!finding.missing_evidence.empty());
    }
  }
  CO_CHECK(separated);
}

CO_TEST(remote_access_rows_carry_locality_generation_and_cost_kind) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  Nanos clock = monotonic_now_ns();
  for (int i = 0; i < 24; ++i) {
    Observation observation = cotest::exact_observation(0, EventType::RemoteRead, clock);
    clock += 1000;
    observation.source = cotest::ref_accelerator("acc.b");
    observation.target = cotest::ref_domain("md.host.a");
    observation.bytes = 4096;
    host->publish(std::move(observation));
  }
  const Result<std::vector<RemoteAccessRow>> rows = observatory.remote_access();
  CO_REQUIRE(rows.ok());
  CO_REQUIRE(!rows.value().empty());
  const RemoteAccessRow& row = rows.value().front();
  CO_CHECK(row.source == "acc.b");
  CO_CHECK(row.target == "md.host.a");
  CO_CHECK_EQ(row.accesses, std::uint64_t{24});
  CO_CHECK_EQ(row.bytes, std::uint64_t{24 * 4096});
  CO_CHECK(row.locality_established);
  CO_CHECK(row.locality == Locality::PeerAccelerator);
  CO_CHECK(row.cost.kind != CostKind::Unknown);
  CO_CHECK(!row.cost.terms.empty());
  CO_CHECK_EQ(row.topology_generation.value(), std::uint64_t{1});
}

CO_TEST(analysis_is_deterministic) {
  auto run = []() {
    Observatory observatory(cotest::test_options());
    cotest::register_test_topology(observatory);
    auto host = cotest::start_host(observatory);
    publish_ownership_alternation(*host, "region.test.1", 12, 20000);
    Nanos clock = monotonic_now_ns();
    for (int i = 0; i < 64; ++i) {
      Observation observation = cotest::exact_observation(0, EventType::RemoteRead, clock);
      clock += 1000;
      observation.bytes = 4096;
      host->publish(std::move(observation));
    }
    std::string rendered;
    const Result<std::vector<Finding>> findings = observatory.findings();
    for (const Finding& finding : findings.value()) {
      rendered.append(render_text(explain(finding)));
    }
    return rendered;
  };
  const std::string first = run();
  const std::string second = run();
  CO_CHECK(!first.empty());
  CO_CHECK(first == second);
}

CO_TEST(hotspot_reports_source_target_concentration) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  Nanos clock = monotonic_now_ns();
  for (int i = 0; i < 64; ++i) {
    Observation observation = cotest::exact_observation(0, EventType::RemoteRead, clock);
    clock += 1000;
    observation.source = cotest::ref_accelerator("acc.b");
    observation.target = cotest::ref_domain("md.host.a");
    host->publish(std::move(observation));
  }
  const Result<std::vector<Finding>> findings = observatory.hotspots();
  CO_REQUIRE(findings.ok());
  bool concentration = false;
  for (const Finding& finding : findings.value()) {
    if (finding.subject_kind == "source_target_pair") {
      concentration = true;
      CO_CHECK(has_reason(finding, finding_reason::kSourceTargetConcentration));
    }
  }
  CO_CHECK(concentration);
}

CO_TEST(cxl_and_remote_numa_traffic_are_named_separately) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  Nanos clock = monotonic_now_ns();
  for (int i = 0; i < 8; ++i) {
    Observation cxl = cotest::exact_observation(0, EventType::RemoteRead, clock);
    clock += 1000;
    cxl.source = cotest::ref_processor("cpu.a");
    cxl.target = cotest::ref_domain("md.cxl");
    cxl.locality = Locality::CxlAttached;
    cxl.locality_declared = true;
    cxl.bytes = 1u << 20;
    host->publish(std::move(cxl));

    Observation remote = cotest::exact_observation(1, EventType::RemoteWrite, clock);
    clock += 1000;
    remote.source = cotest::ref_processor("cpu.b");
    remote.target = cotest::ref_domain("md.host.a");
    remote.locality = Locality::RemoteNuma;
    remote.locality_declared = true;
    remote.bytes = 1u << 20;
    host->publish(std::move(remote));
  }
  const Result<std::vector<Finding>> findings = observatory.hotspots();
  CO_REQUIRE(findings.ok());
  bool saw_cxl = false;
  bool saw_numa = false;
  for (const Finding& finding : findings.value()) {
    if (finding.subject_kind != "locality") {
      continue;
    }
    if (has_reason(finding, finding_reason::kCxlClassTraffic)) {
      saw_cxl = true;
    }
    if (has_reason(finding, finding_reason::kCrossNumaTraffic)) {
      saw_numa = true;
    }
  }
  CO_CHECK(saw_cxl);
  CO_CHECK(saw_numa);
}

}  // namespace