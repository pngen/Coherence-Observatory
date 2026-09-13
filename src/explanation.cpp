// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/explanation.hpp"

#include <algorithm>

#include "text_util.hpp"

namespace sol::coherence {
namespace {

std::string json_escape(std::string_view text) {
  std::string out;
  out.reserve(text.size() + 2);
  for (char c : text) {
    switch (c) {
      case '"': out.append("\\\""); break;
      case '\\': out.append("\\\\"); break;
      case '\n': out.append("\\n"); break;
      case '\r': out.append("\\r"); break;
      case '\t': out.append("\\t"); break;
      default:
        if (static_cast<unsigned char>(c) < 0x20u) {
          out.append("?");
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  return out;
}

void add_cost_terms(Explanation& explanation, const CostEstimate& cost) {
  ExplanationSection& section = explanation.section("cost_decomposition");
  section.add("cost.kind", std::string(to_string(cost.kind)));
  section.add("cost.model_id", cost.model_id.empty() ? std::string("none") : cost.model_id);
  section.add("cost.model_version", static_cast<std::uint64_t>(cost.model_version));
  section.add("cost.latency_ns", cost.latency_ns);
  section.add("cost.bytes", cost.bytes);
  section.add("cost.remote_accesses", cost.remote_accesses);
  section.add("cost.ownership_transfers", cost.ownership_transfers);
  section.add("cost.invalidations", cost.invalidations);
  section.add("cost.retries", cost.retries);
  section.add("cost.stall_cycles", cost.stall_cycles);
  section.add("cost.lower_bound", cost.lower_bound);
  ExplanationSection& terms = explanation.section("cost_terms");
  for (const CostTerm& term : cost.terms) {
    terms.add(term.name,
              detail::format_double(term.value, 6) + " " + term.unit + " (" +
                  std::string(to_string(term.kind)) + ", coefficient " +
                  detail::format_double(term.coefficient, 6) + " x " +
                  detail::format_double(term.quantity, 6) + ")");
  }
}

void add_bindings(Explanation& explanation, const GenerationBindings& bindings) {
  ExplanationSection& section = explanation.section("generations");
  section.add("coordinator_epoch", bindings.coordinator_epoch.value());
  section.add("observation_epoch", bindings.observation_epoch.value());
  section.add("topology_generation", bindings.topology_generation.value());
  section.add("evidence_generation", bindings.evidence_generation.value());
  section.add("source_device_generation", bindings.source_device_generation.value());
  section.add("target_device_generation", bindings.target_device_generation.value());
  section.add("region_generation_bound", bindings.region_generation_bound);
  section.add("region_generation", bindings.region_generation.value());
  section.add("source_domain_generation", bindings.source_domain_generation.value());
  section.add("target_domain_generation", bindings.target_domain_generation.value());
}

void add_evidence(Explanation& explanation, const std::vector<EvidenceRef>& evidence) {
  ExplanationSection& section = explanation.section("evidence");
  std::size_t index = 0;
  for (const EvidenceRef& ref : evidence) {
    const std::string key = "evidence." + detail::format_u64(index);
    section.add(key, "event=" + detail::format_u64(ref.event_id.value()) + " publisher=" +
                         ref.publisher.str() + " boot=" +
                         detail::format_u64(ref.publisher_boot.value()) + " sequence=" +
                         detail::format_u64(ref.sequence.value()) + " type=" +
                         std::string(to_string(ref.event_type)) + " precision=" +
                         std::string(to_string(ref.precision)) + " provenance=" +
                         std::string(to_string(ref.provenance)));
    ++index;
  }
  if (evidence.empty()) {
    section.add("evidence.count", static_cast<std::uint64_t>(0));
  }
}

}  // namespace

void ExplanationSection::add(std::string key, std::string value) {
  lines.push_back(ExplanationLine{std::move(key), std::move(value)});
}

void ExplanationSection::add(std::string key, std::uint64_t value) {
  add(std::move(key), detail::format_u64(value));
}

void ExplanationSection::add(std::string key, double value) {
  add(std::move(key), detail::format_double(value, 6));
}

void ExplanationSection::add(std::string key, bool value) {
  add(std::move(key), std::string(value ? "true" : "false"));
}

ExplanationSection& Explanation::section(std::string title) {
  for (ExplanationSection& existing : sections) {
    if (existing.title == title) {
      return existing;
    }
  }
  sections.push_back(ExplanationSection{std::move(title), {}});
  return sections.back();
}

std::string render_text(const Explanation& explanation) {
  std::string out;
  out.append(explanation.kind);
  if (!explanation.subject.empty()) {
    out.append(" ");
    out.append(explanation.subject);
  }
  out.push_back('\n');
  for (const ExplanationSection& section : explanation.sections) {
    out.append("== ");
    out.append(section.title);
    out.append(" ==\n");
    std::vector<ExplanationLine> lines = section.lines;
    std::stable_sort(lines.begin(), lines.end(),
                     [](const ExplanationLine& a, const ExplanationLine& b) {
                       return a.key < b.key;
                     });
    for (const ExplanationLine& line : lines) {
      out.append("  ");
      out.append(line.key);
      out.append(" = ");
      out.append(line.value);
      out.push_back('\n');
    }
  }
  return out;
}

std::string render_json(const Explanation& explanation) {
  std::string out = "{";
  out.append("\"kind\":\"");
  out.append(json_escape(explanation.kind));
  out.append("\",\"subject\":\"");
  out.append(json_escape(explanation.subject));
  out.append("\",\"sections\":[");
  for (std::size_t i = 0; i < explanation.sections.size(); ++i) {
    const ExplanationSection& section = explanation.sections[i];
    if (i != 0) {
      out.push_back(',');
    }
    out.append("{\"title\":\"");
    out.append(json_escape(section.title));
    out.append("\",\"lines\":[");
    std::vector<ExplanationLine> lines = section.lines;
    std::stable_sort(lines.begin(), lines.end(),
                     [](const ExplanationLine& a, const ExplanationLine& b) {
                       return a.key < b.key;
                     });
    for (std::size_t j = 0; j < lines.size(); ++j) {
      if (j != 0) {
        out.push_back(',');
      }
      out.append("{\"key\":\"");
      out.append(json_escape(lines[j].key));
      out.append("\",\"value\":\"");
      out.append(json_escape(lines[j].value));
      out.append("\"}");
    }
    out.append("]}");
  }
  out.append("]}");
  return out;
}

Explanation explain(const Finding& finding) {
  Explanation explanation;
  explanation.kind = std::string("finding.") + std::string(to_string(finding.kind));
  explanation.subject = finding.subject_kind + ":" + finding.subject;

  ExplanationSection& summary = explanation.section("summary");
  summary.add("finding_kind", std::string(to_string(finding.kind)));
  summary.add("subject_kind", finding.subject_kind);
  summary.add("subject", finding.subject);
  summary.add("historical", finding.historical);
  summary.add("precision", std::string(to_string(finding.precision)));
  summary.add("provenance", std::string(to_string(finding.provenance)));
  summary.add("reality", std::string(to_string(finding.reality)));
  summary.add("evidence_granularity", std::string(to_string(finding.granularity)));
  summary.add("contention_class", std::string(to_string(finding.contention)));
  summary.add("locality", finding.locality_established ? std::string(to_string(finding.locality))
                                                       : std::string("UNKNOWN"));
  summary.add("alternations", finding.alternations);
  if (!finding.direction_sequence.empty()) {
    summary.add("direction_sequence", finding.direction_sequence);
  }

  ExplanationSection& reasons = explanation.section("reasons");
  for (const std::string& reason : finding.reasons) {
    reasons.add(reason, true);
  }

  ExplanationSection& metrics = explanation.section("decisive_metrics");
  for (const FindingMetric& metric : finding.metrics) {
    std::string value = detail::format_double(metric.value, 6);
    if (!metric.unit.empty()) {
      value.append(" ");
      value.append(metric.unit);
    }
    if (metric.has_threshold) {
      value.append(" (threshold ");
      value.append(detail::format_double(metric.threshold, 6));
      value.append(metric.exceeded ? ", exceeded)" : ", within)");
    }
    metrics.add(metric.name, value);
  }

  ExplanationSection& missing = explanation.section("missing_evidence");
  for (std::size_t i = 0; i < finding.missing_evidence.size(); ++i) {
    missing.add("missing." + detail::format_u64(i), finding.missing_evidence[i]);
  }
  if (finding.missing_evidence.empty()) {
    missing.add("missing.count", static_cast<std::uint64_t>(0));
  }

  ExplanationSection& ambiguity = explanation.section("ambiguity");
  for (std::size_t i = 0; i < finding.ambiguity.size(); ++i) {
    ambiguity.add("candidate." + detail::format_u64(i), finding.ambiguity[i]);
  }
  if (finding.ambiguity.empty()) {
    ambiguity.add("ambiguity.count", static_cast<std::uint64_t>(0));
  }

  ExplanationSection& participants = explanation.section("participants");
  for (std::size_t i = 0; i < finding.participants.size(); ++i) {
    participants.add("participant." + detail::format_u64(i), finding.participants[i]);
  }

  add_evidence(explanation, finding.evidence);
  add_bindings(explanation, finding.bindings);
  add_cost_terms(explanation, finding.cost);
  return explanation;
}

Explanation explain(const AttributionResult& attribution) {
  Explanation explanation;
  explanation.kind = "attribution";
  explanation.subject =
      attribution.region.empty() ? std::string("unbound") : attribution.region.str();

  ExplanationSection& summary = explanation.section("summary");
  summary.add("outcome", std::string(to_string(attribution.outcome)));
  summary.add("attribution_id", attribution.id.value());
  summary.add("event_type", std::string(to_string(attribution.event_type)));
  summary.add("precision", std::string(to_string(attribution.precision)));
  summary.add("provenance", std::string(to_string(attribution.provenance)));
  summary.add("reality", std::string(to_string(attribution.reality)));
  summary.add("candidate_count", attribution.candidate_count);
  if (attribution.locality_established) {
    summary.add("locality", std::string(to_string(attribution.locality)));
  }

  ExplanationSection& reasons = explanation.section("reasons");
  for (const std::string& reason : attribution.reasons) {
    reasons.add(reason, true);
  }

  ExplanationSection& targets = explanation.section("targets");
  for (std::size_t i = 0; i < attribution.targets.size(); ++i) {
    const AttributionTarget& target = attribution.targets[i];
    targets.add("target." + detail::format_u64(i),
                std::string(to_string(target.kind)) + ":" + target.value + "@" +
                    detail::format_u64(target.generation));
  }

  ExplanationSection& candidates = explanation.section("candidates");
  for (std::size_t i = 0; i < attribution.candidates.size(); ++i) {
    candidates.add("candidate." + detail::format_u64(i), attribution.candidates[i]);
  }

  add_evidence(explanation, attribution.evidence);
  add_bindings(explanation, attribution.bindings);

  ExplanationSection& cost = explanation.section("cost_decomposition");
  cost.add("cost.kind", std::string(to_string(attribution.cost.kind)));
  cost.add("cost.latency_ns", attribution.cost.latency_ns);
  cost.add("cost.bytes", attribution.cost.bytes);

  return explanation;
}

Explanation explain(const CostEstimate& cost) {
  Explanation explanation;
  explanation.kind = "cost";
  explanation.subject = cost.model_id.empty() ? std::string("unmodelled") : cost.model_id;
  add_cost_terms(explanation, cost);
  ExplanationSection& summary = explanation.section("summary");
  summary.add("precision", std::string(to_string(cost.precision)));
  summary.add("bandwidth_bytes_per_ns", cost.bandwidth_bytes_per_ns);
  return explanation;
}

Explanation explain_loss(const LossReport& loss) {
  Explanation explanation;
  explanation.kind = "evidence_loss";
  explanation.subject = "observatory";
  ExplanationSection& section = explanation.section("loss");
  section.add("rejected_observations", loss.rejected_observations);
  section.add("rejected_duplicates", loss.rejected_duplicates);
  section.add("rejected_conflicting_duplicates", loss.rejected_conflicting_duplicates);
  section.add("rejected_stale_sequences", loss.rejected_stale_sequences);
  section.add("rejected_unauthorized", loss.rejected_unauthorized);
  section.add("rejected_malformed", loss.rejected_malformed);
  section.add("rejected_overflow", loss.rejected_overflow);
  section.add("dropped_aggregate_updates", loss.dropped_aggregate_updates);
  section.add("dropped_journal_entries", loss.dropped_journal_entries);
  section.add("dropped_region_samples", loss.dropped_region_samples);
  section.add("evicted_history_records", loss.evicted_history_records);
  section.add("missing_sequences", loss.missing_sequences);
  section.add("backpressure_rejections", loss.backpressure_rejections);
  section.add("total_rejected", loss.total_rejected());
  return explanation;
}

}  // namespace sol::coherence
