// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "test_framework.hpp"
#include "test_support.hpp"

#include <string>

#include "coherence/explanation.hpp"
#include "coherence/observatory.hpp"

namespace {

using namespace sol::coherence;

CO_TEST(explanation_rendering_is_stable_and_structured) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  for (int i = 0; i < 40; ++i) {
    Observation observation = cotest::exact_observation(0, EventType::RemoteRead,
                                                        static_cast<Nanos>(i * 1000));
    observation.bytes = 4096;
    host->publish(std::move(observation));
  }
  const Result<std::vector<Finding>> findings = observatory.findings();
  CO_REQUIRE(findings.ok());
  CO_REQUIRE(!findings.value().empty());
  const std::string first = render_text(explain(findings.value().front()));
  const std::string second = render_text(explain(findings.value().front()));
  CO_CHECK(first == second);
  CO_CHECK(first.find("== summary ==") != std::string::npos);
  CO_CHECK(first.find("precision") != std::string::npos);
  CO_CHECK(first.find("reality") != std::string::npos);
  CO_CHECK(first.find("generations") != std::string::npos);
  CO_CHECK(first.find("cost_decomposition") != std::string::npos);
}

CO_TEST(explanation_json_is_well_formed_and_escapes) {
  Explanation explanation;
  explanation.kind = "test";
  explanation.subject = "subject \"quoted\"\\slash";
  ExplanationSection& section = explanation.section("section");
  section.add("b", std::string("second"));
  section.add("a", std::string("first"));
  const std::string json = render_json(explanation);
  CO_CHECK(json.find("\"kind\":\"test\"") != std::string::npos);
  CO_CHECK(json.find("\\\"quoted\\\"") != std::string::npos);
  CO_CHECK(json.find("\\\\slash") != std::string::npos);
  const std::size_t first = json.find("\"key\":\"a\"");
  const std::size_t second = json.find("\"key\":\"b\"");
  CO_CHECK(first != std::string::npos);
  CO_CHECK(second != std::string::npos);
  CO_CHECK(first < second);
}

CO_TEST(loss_explanation_reports_every_counter) {
  LossReport loss;
  loss.rejected_observations = 1;
  loss.rejected_duplicates = 2;
  loss.missing_sequences = 3;
  const Explanation explanation = explain_loss(loss);
  const std::string text = render_text(explanation);
  CO_CHECK(text.find("rejected_observations") != std::string::npos);
  CO_CHECK(text.find("missing_sequences") != std::string::npos);
  CO_CHECK(text.find("total_rejected = 6") != std::string::npos);
}

CO_TEST(snapshot_is_immutable_after_capture) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  host->publish(cotest::exact_observation(0, EventType::RemoteRead, 1000));
  const SnapshotPtr captured = observatory.snapshot();
  CO_CHECK_EQ(captured->regions().size(), std::size_t{4});
  CO_CHECK_EQ(captured->aggregates().observations_in(AggregateDimension::EventType),
              std::uint64_t{1});

  for (int i = 0; i < 32; ++i) {
    host->publish(cotest::exact_observation(1, EventType::RemoteWrite,
                                            static_cast<Nanos>(2000 + i * 10)));
  }
  CO_CHECK_EQ(captured->aggregates().observations_in(AggregateDimension::EventType),
              std::uint64_t{1});
  CO_CHECK_EQ(captured->publishers().size(), std::size_t{1});
  CO_CHECK_EQ(captured->publishers().front().accepted_events, std::uint64_t{1});
}

CO_TEST(snapshot_currentness_is_explicit) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  const SnapshotPtr before = observatory.snapshot();
  CO_CHECK(before->is_current(observatory.coordinator_epoch(), observatory.observation_epoch()));
  observatory.reconcile_current_evidence();
  CO_CHECK(!before->is_current(observatory.coordinator_epoch(), observatory.observation_epoch()));
}

CO_TEST(snapshot_generation_advances_per_capture) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  const SnapshotPtr first = observatory.snapshot();
  const SnapshotPtr second = observatory.snapshot();
  CO_CHECK(second->generation().value() > first->generation().value());
}

CO_TEST(snapshot_exposes_supporting_evidence_and_classification) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  host->publish(cotest::exact_observation(0, EventType::RemoteRead, 1000));
  observatory.retire_region(MemoryRegionId{"region.test.1"}, MemoryRegionGeneration{1},
                            "retire for test");
  host->publish(cotest::exact_observation(1, EventType::RemoteRead, 2000));
  const SnapshotPtr snapshot = observatory.snapshot();
  CO_CHECK(!snapshot->stale_evidence().empty());
  CO_CHECK(!snapshot->capabilities().empty());
  bool saw_unsupported = false;
  for (const Capability& capability : snapshot->capabilities()) {
    if (capability.status == CapabilityStatus::Unsupported) {
      saw_unsupported = true;
      CO_CHECK(!capability.detail.empty());
    }
  }
  CO_CHECK(saw_unsupported);
  // Evidence loss is explicit: a malformed observation is counted as loss.
  Observation malformed = cotest::exact_observation(0, EventType::RemoteRead, 3000);
  malformed.bytes = Limits::kMaxByteCount + 1;
  host->publish(malformed);
  CO_CHECK(observatory.snapshot()->loss().loss.total_rejected() > 0);
}

CO_TEST(attribution_explanation_lists_targets_and_generations) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  auto host = cotest::start_host(observatory);
  host->publish(cotest::exact_observation(0, EventType::RemoteRead, 1000));
  const Result<AttributionResult> attribution =
      observatory.attribute_region(MemoryRegionId{"region.test.0"});
  CO_REQUIRE(attribution.ok());
  const std::string text = render_text(explain(attribution.value()));
  CO_CHECK(text.find("outcome = ATTRIBUTED_EXACT") != std::string::npos);
  CO_CHECK(text.find("== targets ==") != std::string::npos);
  CO_CHECK(text.find("region_generation = 1") != std::string::npos);
}

CO_TEST(region_analysis_reports_granularity_and_missing_evidence) {
  Observatory observatory(cotest::test_options());
  cotest::register_test_topology(observatory);
  const Result<RegionAnalysis> analysis =
      observatory.analyze_region(MemoryRegionId{"region.test.2"});
  CO_REQUIRE(analysis.ok());
  CO_CHECK(analysis.value().registered);
  CO_CHECK_EQ(analysis.value().observations, std::uint64_t{0});
  CO_CHECK(!analysis.value().missing_evidence.empty());
  const Result<RegionAnalysis> missing =
      observatory.analyze_region(MemoryRegionId{"region.absent"});
  CO_REQUIRE(missing.ok());
  CO_CHECK(!missing.value().registered);
}

}  // namespace