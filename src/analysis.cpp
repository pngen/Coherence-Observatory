// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Analyzers.  Every analyzer is a pure function of state: it takes no lock,
// performs no I/O, and produces findings whose ordering is fully determined by
// their content.

#include "detail/state.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "coherence/explanation.hpp"
#include "detail/cost.hpp"
#include "text_util.hpp"

namespace sol::coherence {
namespace {

using detail::OwnershipSample;
using detail::State;

EvidenceRef evidence_ref(const Observation& observation) {
  EvidenceRef ref;
  ref.event_id = observation.event_id;
  ref.publisher = observation.source_publisher;
  ref.publisher_boot = observation.publisher_boot;
  ref.sequence = observation.sequence;
  ref.event_type = observation.type;
  ref.precision = observation.precision;
  ref.provenance = observation.provenance;
  return ref;
}

EvidenceRef evidence_ref(const OwnershipSample& sample) {
  EvidenceRef ref;
  ref.event_id = sample.event_id;
  ref.publisher = sample.publisher;
  ref.publisher_boot = sample.publisher_boot;
  ref.sequence = sample.sequence;
  ref.event_type = sample.type;
  ref.precision = sample.precision;
  ref.provenance = sample.provenance;
  return ref;
}

void push_evidence(std::vector<EvidenceRef>& out, const EvidenceRef& ref) {
  if (out.size() >= Limits::kMaxEvidenceRefs) {
    return;
  }
  out.push_back(ref);
}

void add_reason(Finding& finding, std::string_view reason) {
  const std::string text(reason);
  if (std::find(finding.reasons.begin(), finding.reasons.end(), text) == finding.reasons.end()) {
    finding.reasons.push_back(text);
  }
}

void add_metric(Finding& finding, std::string name, double value, std::string unit,
                double threshold, bool has_threshold) {
  FindingMetric metric;
  metric.name = std::move(name);
  metric.value = value;
  metric.unit = std::move(unit);
  metric.threshold = threshold;
  metric.has_threshold = has_threshold;
  metric.exceeded = has_threshold && value >= threshold;
  finding.metrics.push_back(std::move(metric));
}

void add_missing(Finding& finding, std::string text) {
  if (finding.missing_evidence.size() >= Limits::kMaxExplanationLines) {
    return;
  }
  if (std::find(finding.missing_evidence.begin(), finding.missing_evidence.end(), text) ==
      finding.missing_evidence.end()) {
    finding.missing_evidence.push_back(std::move(text));
  }
}

void add_ambiguity(Finding& finding, std::string text) {
  if (finding.ambiguity.size() >= Limits::kMaxAttributionCandidates) {
    return;
  }
  if (std::find(finding.ambiguity.begin(), finding.ambiguity.end(), text) ==
      finding.ambiguity.end()) {
    finding.ambiguity.push_back(std::move(text));
  }
}

void add_participant(Finding& finding, const std::string& text) {
  if (finding.participants.size() >= Limits::kMaxAttributionCandidates) {
    return;
  }
  if (std::find(finding.participants.begin(), finding.participants.end(), text) ==
      finding.participants.end()) {
    finding.participants.push_back(text);
  }
}

GenerationBindings current_bindings(const State& state) {
  GenerationBindings bindings;
  bindings.coordinator_epoch = state.coordinator_epoch;
  bindings.observation_epoch = state.observation_epoch;
  bindings.topology_generation = state.topology_generation;
  bindings.evidence_generation = state.evidence_generation;
  return bindings;
}

void sort_finding(Finding& finding) {
  std::sort(finding.reasons.begin(), finding.reasons.end());
  finding.reasons.erase(std::unique(finding.reasons.begin(), finding.reasons.end()),
                        finding.reasons.end());
  std::sort(finding.metrics.begin(), finding.metrics.end(),
            [](const FindingMetric& a, const FindingMetric& b) { return a.name < b.name; });
  std::sort(finding.participants.begin(), finding.participants.end());
  finding.participants.erase(
      std::unique(finding.participants.begin(), finding.participants.end()),
      finding.participants.end());
}

double rate_over_span(std::uint64_t count, Nanos first, Nanos last) {
  if (last <= first) {
    return 0.0;
  }
  const double span_ns = static_cast<double>(last - first);
  return static_cast<double>(count) * 1.0e9 / span_ns;
}

}  // namespace

std::string Finding::ordering_key() const {
  std::string key(to_string(kind));
  key.push_back('|');
  key.append(subject_kind);
  key.push_back('|');
  key.append(subject);
  return key;
}

std::string_view to_string(FindingKind kind) noexcept {
  switch (kind) {
    case FindingKind::Hotspot: return "HOTSPOT";
    case FindingKind::PingPong: return "PING_PONG";
    case FindingKind::FalseSharingLike: return "FALSE_SHARING_LIKE";
    case FindingKind::InvalidationBurst: return "INVALIDATION_BURST";
    case FindingKind::RemoteAccessConcentration: return "REMOTE_ACCESS_CONCENTRATION";
    case FindingKind::CounterDiscontinuity: return "COUNTER_DISCONTINUITY";
    case FindingKind::SequenceGap: return "SEQUENCE_GAP";
    case FindingKind::EvidenceLoss: return "EVIDENCE_LOSS";
    case FindingKind::StaleEvidence: return "STALE_EVIDENCE";
    case FindingKind::CapacityPressure: return "CAPACITY_PRESSURE";
    case FindingKind::PublisherLoss: return "PUBLISHER_LOSS";
    case FindingKind::UnsupportedObservability: return "UNSUPPORTED_OBSERVABILITY";
  }
  return "HOTSPOT";
}

Result<FindingKind> parse_finding_kind(std::string_view text) {
  static const std::pair<std::string_view, FindingKind> table[] = {
      {"HOTSPOT", FindingKind::Hotspot},
      {"PING_PONG", FindingKind::PingPong},
      {"FALSE_SHARING_LIKE", FindingKind::FalseSharingLike},
      {"INVALIDATION_BURST", FindingKind::InvalidationBurst},
      {"REMOTE_ACCESS_CONCENTRATION", FindingKind::RemoteAccessConcentration},
      {"COUNTER_DISCONTINUITY", FindingKind::CounterDiscontinuity},
      {"SEQUENCE_GAP", FindingKind::SequenceGap},
      {"EVIDENCE_LOSS", FindingKind::EvidenceLoss},
      {"STALE_EVIDENCE", FindingKind::StaleEvidence},
      {"CAPACITY_PRESSURE", FindingKind::CapacityPressure},
      {"PUBLISHER_LOSS", FindingKind::PublisherLoss},
      {"UNSUPPORTED_OBSERVABILITY", FindingKind::UnsupportedObservability},
  };
  for (const auto& entry : table) {
    if (detail::iequals(entry.first, text)) {
      return Result<FindingKind>(entry.second);
    }
  }
  return Result<FindingKind>(
      Error(ErrorCode::InvalidArgument, "unknown finding kind", std::string(text)));
}

std::string_view to_string(ContentionClass value) noexcept {
  switch (value) {
    case ContentionClass::None: return "NONE";
    case ContentionClass::CacheLineFalseSharingSupported:
      return "CACHE_LINE_FALSE_SHARING_SUPPORTED";
    case ContentionClass::RegionContentionLikely: return "REGION_CONTENTION_LIKELY";
    case ContentionClass::PageLevelContention: return "PAGE_LEVEL_CONTENTION";
    case ContentionClass::InsufficientGranularity: return "INSUFFICIENT_GRANULARITY";
  }
  return "NONE";
}

namespace detail {

std::vector<Finding> analyze_hotspots(const State& state, const HotspotPolicy& policy) {
  std::vector<Finding> findings;
  const std::uint64_t total_region_bytes = state.aggregates.bytes_in(AggregateDimension::Region);
  const std::uint64_t total_pair_observations =
      state.aggregates.observations_in(AggregateDimension::SourceTargetPair);

  // One pass over the bucket set, dispatched by dimension.  Reason conditions
  // are evaluated from the raw accumulated values first, so that a bucket that
  // is not a hotspot never allocates a finding, its metrics or its cost
  // decomposition.  The produced findings are identical to building every
  // candidate and discarding the ones without reasons.
  const std::size_t per_subject_cap = policy.max_hotspots * 4;
  std::uint64_t max_pair = 0;
  std::string max_pair_name;
  Reality max_pair_reality = Reality::Real;

  for (const auto& entry : state.aggregates.buckets()) {
    const AggregateValue& bucket = entry.second;
    const AggregateDimension dimension = entry.first.dimension;

    if (dimension == AggregateDimension::Region && entry.first.value != "unknown") {
      const double read_rate = rate_over_span(bucket.remote_reads, bucket.first_timestamp_ns,
                                              bucket.last_timestamp_ns);
      const double write_rate = rate_over_span(bucket.remote_writes, bucket.first_timestamp_ns,
                                               bucket.last_timestamp_ns);
      const bool high_read = read_rate >= policy.min_remote_read_rate_per_second &&
                             bucket.remote_reads >= policy.min_remote_accesses;
      const bool high_write = write_rate >= policy.min_remote_write_rate_per_second &&
                              bucket.remote_writes >= policy.min_remote_accesses;
      const bool transfers = bucket.ownership_transfers >= policy.min_ownership_transfers;
      const bool invalidations = bucket.invalidations >= policy.min_invalidations;
      const bool contention =
          bucket.ownership_transfers + bucket.invalidations >= policy.min_region_events;
      double share = 0.0;
      bool disproportionate = false;
      if (total_region_bytes > 0 && bucket.bytes > 0) {
        share = static_cast<double>(bucket.bytes) / static_cast<double>(total_region_bytes);
        disproportionate = share >= policy.min_coherence_byte_share;
      }
      if (!(high_read || high_write || transfers || invalidations || contention ||
            disproportionate) ||
          findings.size() >= per_subject_cap) {
        continue;
      }

      Finding finding;
      finding.kind = FindingKind::Hotspot;
      finding.subject_kind = "region";
      finding.subject = entry.first.value;
      finding.precision = bucket.weakest_precision;
      finding.provenance = Provenance::DerivedAggregation;
      finding.reality = bucket.reality();
      finding.bindings = current_bindings(state);

      const auto region_it = state.regions.find(entry.first.value);
      if (region_it != state.regions.end()) {
        finding.bindings.region_generation = region_it->second.generation;
        finding.bindings.region_generation_bound = true;
        add_metric(finding, "region.generation",
                   static_cast<double>(region_it->second.generation.value()), "generation", 0.0,
                   false);
      } else {
        add_missing(finding, "region.registration");
      }

      add_metric(finding, "remote_read.count", static_cast<double>(bucket.remote_reads),
                 "accesses", 0.0, false);
      add_metric(finding, "remote_read.rate_per_second", read_rate, "1/s",
                 policy.min_remote_read_rate_per_second, true);
      add_metric(finding, "remote_write.count", static_cast<double>(bucket.remote_writes),
                 "accesses", 0.0, false);
      add_metric(finding, "remote_write.rate_per_second", write_rate, "1/s",
                 policy.min_remote_write_rate_per_second, true);
      add_metric(finding, "remote_access.count", static_cast<double>(bucket.remote_accesses),
                 "accesses", static_cast<double>(policy.min_remote_accesses), true);
      add_metric(finding, "bytes", static_cast<double>(bucket.bytes), "bytes", 0.0, false);
      add_metric(finding, "ownership_transfer.count",
                 static_cast<double>(bucket.ownership_transfers), "transfers",
                 static_cast<double>(policy.min_ownership_transfers), true);
      add_metric(finding, "invalidation.count", static_cast<double>(bucket.invalidations),
                 "invalidations", static_cast<double>(policy.min_invalidations), true);
      add_metric(finding, "observations", static_cast<double>(bucket.observations),
                 "observations", 0.0, false);
      if (high_read) {
        add_reason(finding, finding_reason::kHighRemoteReadRate);
      }
      if (high_write) {
        add_reason(finding, finding_reason::kHighRemoteWriteRate);
      }
      if (transfers) {
        add_reason(finding, finding_reason::kRepeatedOwnershipTransfer);
      }
      if (invalidations) {
        add_reason(finding, finding_reason::kInvalidationBurst);
      }
      if (contention) {
        add_reason(finding, finding_reason::kRegionLevelContention);
      }
      if (disproportionate) {
        add_metric(finding, "coherence_byte_share", share, "share",
                   policy.min_coherence_byte_share, true);
        add_reason(finding, finding_reason::kDisproportionateCoherenceBytes);
      }

      CostInputs inputs;
      inputs.bytes = bucket.bytes;
      inputs.remote_reads = bucket.remote_reads;
      inputs.remote_writes = bucket.remote_writes;
      inputs.invalidations = bucket.invalidations;
      inputs.ownership_transfers = bucket.ownership_transfers;
      inputs.retries = bucket.retries;
      inputs.conflicts = bucket.conflicts;
      inputs.precision = bucket.weakest_precision;
      finding.cost = estimate_cost(state.options.cost_model, inputs);

      const auto evidence_it = state.region_evidence.find(entry.first.value);
      if (evidence_it != state.region_evidence.end()) {
        const RegionEvidence& evidence = evidence_it->second;
        if (evidence.evicted != 0) {
          add_missing(finding, "region sample window evicted " +
                                   detail::format_u64(evidence.evicted) + " older samples");
        }
        for (const OwnershipSample& sample : evidence.samples) {
          push_evidence(finding.evidence, evidence_ref(sample));
        }
        if (!evidence.samples.empty()) {
          finding.granularity = evidence.samples.back().granularity;
        }
      } else {
        add_missing(finding, "per-region pattern samples");
        finding.granularity = EvidenceGranularity::Aggregate;
      }
      sort_finding(finding);
      findings.push_back(std::move(finding));
      continue;
    }

    if (dimension == AggregateDimension::Device && entry.first.value != "unknown") {
      const bool busy = bucket.remote_accesses >= policy.min_remote_accesses ||
                        bucket.ownership_transfers >= policy.min_ownership_transfers ||
                        bucket.invalidations >= policy.min_invalidations;
      if (!busy || findings.size() >= per_subject_cap) {
        continue;
      }
      Finding finding;
      finding.kind = FindingKind::Hotspot;
      finding.subject_kind = "device";
      finding.subject = entry.first.value;
      finding.precision = bucket.weakest_precision;
      finding.provenance = Provenance::DerivedAggregation;
      finding.reality = bucket.reality();
      finding.bindings = current_bindings(state);
      finding.granularity = EvidenceGranularity::Device;
      add_metric(finding, "observations", static_cast<double>(bucket.observations),
                 "observations", 0.0, false);
      add_metric(finding, "remote_access.count", static_cast<double>(bucket.remote_accesses),
                 "accesses", static_cast<double>(policy.min_remote_accesses), true);
      add_metric(finding, "ownership_transfer.count",
                 static_cast<double>(bucket.ownership_transfers), "transfers",
                 static_cast<double>(policy.min_ownership_transfers), true);
      add_metric(finding, "invalidation.count", static_cast<double>(bucket.invalidations),
                 "invalidations", static_cast<double>(policy.min_invalidations), true);
      add_metric(finding, "bytes", static_cast<double>(bucket.bytes), "bytes", 0.0, false);
      if (bucket.remote_accesses >= policy.min_remote_accesses) {
        add_reason(finding, finding_reason::kHighRemoteReadRate);
      }
      if (bucket.ownership_transfers >= policy.min_ownership_transfers) {
        add_reason(finding, finding_reason::kRepeatedOwnershipTransfer);
      }
      if (bucket.invalidations >= policy.min_invalidations) {
        add_reason(finding, finding_reason::kInvalidationBurst);
      }
      CostInputs inputs;
      inputs.bytes = bucket.bytes;
      inputs.remote_reads = bucket.remote_reads;
      inputs.remote_writes = bucket.remote_writes;
      inputs.invalidations = bucket.invalidations;
      inputs.ownership_transfers = bucket.ownership_transfers;
      inputs.retries = bucket.retries;
      inputs.conflicts = bucket.conflicts;
      inputs.precision = bucket.weakest_precision;
      finding.cost = estimate_cost(state.options.cost_model, inputs);
      sort_finding(finding);
      findings.push_back(std::move(finding));
      continue;
    }

    if (dimension == AggregateDimension::Locality && entry.first.value != "unknown") {
      bool exceeded = false;
      switch (parse_locality(entry.first.value).value_or(Locality::Unknown)) {
        case Locality::RemoteNuma:
        case Locality::RemoteNode:
          exceeded = bucket.bytes >= policy.min_cross_numa_bytes;
          break;
        case Locality::RemoteAccelerator:
        case Locality::PeerAccelerator:
          exceeded = bucket.bytes >= policy.min_cross_device_bytes;
          break;
        case Locality::CxlAttached:
        case Locality::PooledMemory:
          exceeded = bucket.bytes >= policy.min_cxl_class_bytes;
          break;
        default:
          exceeded = bucket.bytes >= policy.min_cross_domain_bytes;
          break;
      }
      if (!exceeded || findings.size() >= per_subject_cap) {
        continue;
      }
      Finding finding;
      finding.kind = FindingKind::Hotspot;
      finding.subject_kind = "locality";
      finding.subject = entry.first.value;
      finding.precision = bucket.weakest_precision;
      finding.provenance = Provenance::DerivedAggregation;
      finding.reality = bucket.reality();
      finding.bindings = current_bindings(state);
      const Result<Locality> parsed = parse_locality(entry.first.value);
      if (parsed.ok()) {
        finding.locality = parsed.value();
        finding.locality_established = true;
      }
      add_metric(finding, "bytes", static_cast<double>(bucket.bytes), "bytes", 0.0, false);
      add_metric(finding, "observations", static_cast<double>(bucket.observations),
                 "observations", 0.0, false);
      if (parsed.ok()) {
        switch (parsed.value()) {
          case Locality::RemoteNuma:
          case Locality::RemoteNode:
            add_metric(finding, "cross_numa.bytes", static_cast<double>(bucket.bytes), "bytes",
                       static_cast<double>(policy.min_cross_numa_bytes), true);
            add_reason(finding, finding_reason::kCrossNumaTraffic);
            break;
          case Locality::RemoteAccelerator:
          case Locality::PeerAccelerator:
            add_metric(finding, "cross_device.bytes", static_cast<double>(bucket.bytes), "bytes",
                       static_cast<double>(policy.min_cross_device_bytes), true);
            add_reason(finding, finding_reason::kCrossDeviceTraffic);
            break;
          case Locality::CxlAttached:
          case Locality::PooledMemory:
            add_metric(finding, "cxl_class.bytes", static_cast<double>(bucket.bytes), "bytes",
                       static_cast<double>(policy.min_cxl_class_bytes), true);
            add_reason(finding, finding_reason::kCxlClassTraffic);
            break;
          default:
            add_metric(finding, "cross_domain.bytes", static_cast<double>(bucket.bytes), "bytes",
                       static_cast<double>(policy.min_cross_domain_bytes), true);
            add_reason(finding, finding_reason::kCrossDomainTraffic);
            break;
        }
      }
      CostInputs inputs;
      inputs.bytes = bucket.bytes;
      inputs.remote_reads = bucket.remote_reads;
      inputs.remote_writes = bucket.remote_writes;
      inputs.invalidations = bucket.invalidations;
      inputs.ownership_transfers = bucket.ownership_transfers;
      inputs.precision = bucket.weakest_precision;
      inputs.locality = finding.locality;
      inputs.locality_known = finding.locality_established;
      finding.cost = estimate_cost(state.options.cost_model, inputs);
      sort_finding(finding);
      findings.push_back(std::move(finding));
      continue;
    }

    if (dimension == AggregateDimension::SourceTargetPair &&
        bucket.observations > max_pair) {
      max_pair = bucket.observations;
      max_pair_name = entry.first.value;
      max_pair_reality = bucket.reality();
    }
  }

  // ---- Source/target concentration -------------------------------------
  if (total_pair_observations > 0 && !max_pair_name.empty()) {
    const double concentration =
        static_cast<double>(max_pair) / static_cast<double>(total_pair_observations);
    if (concentration >= policy.min_source_target_concentration) {
      Finding finding;
      finding.kind = FindingKind::Hotspot;
      finding.subject_kind = "source_target_pair";
      finding.subject = max_pair_name;
      finding.precision = Precision::AggregatedCounter;
      finding.provenance = Provenance::DerivedAggregation;
      finding.reality = max_pair_reality;
      finding.bindings = current_bindings(state);
      finding.granularity = EvidenceGranularity::Aggregate;
      add_reason(finding, finding_reason::kSourceTargetConcentration);
      add_metric(finding, "pair.observations", static_cast<double>(max_pair), "observations", 0.0,
                 false);
      add_metric(finding, "source_target_concentration", concentration, "share",
                 policy.min_source_target_concentration, true);
      add_metric(finding, "pair.total_observations",
                 static_cast<double>(total_pair_observations), "observations", 0.0, false);
      sort_finding(finding);
      findings.push_back(std::move(finding));
    }
  }

  finalize_findings(findings, policy.max_hotspots);
  return findings;
}

std::vector<Finding> analyze_ping_pong(const State& state, const PingPongPolicy& policy) {
  std::vector<Finding> findings;
  for (const auto& entry : state.region_evidence) {
    const RegionEvidence& evidence = entry.second;
    std::vector<OwnershipSample> samples;
    for (const OwnershipSample& sample : evidence.samples) {
      if (!is_ownership_event(sample.type)) {
        continue;
      }
      if (precision_rank(sample.precision) < precision_rank(policy.min_precision)) {
        continue;
      }
      if (sample.from.empty() || sample.to.empty()) {
        continue;
      }
      samples.push_back(sample);
    }
    if (samples.size() < 2) {
      continue;
    }
    std::stable_sort(samples.begin(), samples.end(),
                     [](const OwnershipSample& a, const OwnershipSample& b) {
                       if (a.timestamp_ns != b.timestamp_ns) {
                         return a.timestamp_ns < b.timestamp_ns;
                       }
                       return a.sequence.value() < b.sequence.value();
                     });

    std::size_t best_start = 0;
    std::size_t best_length = 0;
    std::size_t start = 0;
    auto alternates = [&samples](std::size_t index) {
      if (index == 0) {
        return true;
      }
      const OwnershipSample& previous = samples[index - 1];
      const OwnershipSample& current = samples[index];
      const bool forward = previous.from.id == current.to.id && previous.to.id == current.from.id;
      const bool reverse = previous.from.id == current.from.id && previous.to.id == current.to.id;
      return forward || reverse;
    };
    for (std::size_t i = 0; i < samples.size(); ++i) {
      if (i == 0 || !alternates(i) ||
          samples[i].timestamp_ns - samples[i - 1].timestamp_ns > policy.max_gap_ns) {
        start = i;
      }
      const std::size_t length = i - start + 1;
      if (length > best_length) {
        best_length = length;
        best_start = start;
      }
    }
    if (best_length < static_cast<std::size_t>(policy.min_alternations) + 1) {
      continue;
    }

    // Trim the window to the policy window measured backwards from the end.
    std::size_t window_start = best_start;
    const Nanos window_end = samples[best_start + best_length - 1].timestamp_ns;
    while (window_start < best_start + best_length &&
           window_end - samples[window_start].timestamp_ns > policy.window_ns) {
      ++window_start;
    }
    const std::size_t window_length = best_start + best_length - window_start;
    if (window_length < static_cast<std::size_t>(policy.min_alternations) + 1) {
      continue;
    }

    std::set<std::string> participants;
    for (std::size_t i = window_start; i < window_start + window_length; ++i) {
      participants.insert(samples[i].from.id.str());
      participants.insert(samples[i].to.id.str());
    }
    if (participants.size() < policy.min_participants) {
      continue;
    }

    Finding finding;
    finding.kind = FindingKind::PingPong;
    finding.subject_kind = "region";
    finding.subject = entry.first;
    finding.bindings = current_bindings(state);
    const auto region_it = state.regions.find(entry.first);
    if (region_it != state.regions.end()) {
      finding.bindings.region_generation = region_it->second.generation;
      finding.bindings.region_generation_bound = true;
    } else {
      add_missing(finding, "region.registration");
    }
    finding.alternations = static_cast<std::uint64_t>(window_length - 1);
    finding.granularity = samples[window_start].granularity;

    Precision weakest_precision = Precision::ExactEvent;
    Provenance provenance = samples[window_start].provenance;
    Reality reality = reality_of(provenance);
    std::uint64_t transfers = 0;
    std::uint64_t bytes_estimate = 0;
    for (std::size_t i = window_start; i < window_start + window_length; ++i) {
      const OwnershipSample& sample = samples[i];
      weakest_precision = weakest(weakest_precision, sample.precision);
      reality = combine_reality(reality, reality_of(sample.provenance));
      add_participant(finding, sample.from.id.str());
      add_participant(finding, sample.to.id.str());
      push_evidence(finding.evidence, evidence_ref(sample));
      if (i > window_start) {
        finding.direction_sequence.push_back(' ');
      }
      finding.direction_sequence.append(sample.from.id.view());
      finding.direction_sequence.push_back('>');
      finding.direction_sequence.append(sample.to.id.view());
      // The rendered sequence is bounded so that the result always fits the
      // wire field; the alternation count carries the full information.
      if (finding.direction_sequence.size() > 160) {
        finding.direction_sequence = detail::truncate(finding.direction_sequence, 160);
        finding.missing_evidence.push_back(
            "direction sequence truncated at 160 characters; the alternation count and "
            "evidence references carry the complete pattern");
        break;
      }
      ++transfers;
      bytes_estimate += 64;
    }
    finding.precision = weakest_precision;
    finding.provenance = provenance;
    finding.reality = reality;
    add_reason(finding, finding_reason::kAlternatingOwnership);
    add_metric(finding, "alternations", static_cast<double>(finding.alternations), "alternations",
               static_cast<double>(policy.min_alternations), true);
    add_metric(finding, "ownership_transfers", static_cast<double>(transfers), "transfers", 0.0,
               false);
    add_metric(finding, "participants", static_cast<double>(participants.size()), "resources",
               static_cast<double>(policy.min_participants), true);

    CostInputs inputs;
    inputs.ownership_transfers = transfers;
    inputs.bytes = bytes_estimate;
    inputs.precision = weakest_precision;
    finding.cost = estimate_cost(state.options.cost_model, inputs);

    if (evidence.evicted != 0) {
      add_missing(finding, "region sample window evicted " + detail::format_u64(evidence.evicted) +
                               " older samples");
    }
    sort_finding(finding);
    findings.push_back(std::move(finding));
  }
  finalize_findings(findings, policy.max_findings);
  return findings;
}

std::vector<Finding> analyze_false_sharing(const State& state, const FalseSharingPolicy& policy) {
  std::vector<Finding> findings;
  for (const auto& entry : state.region_evidence) {
    const RegionEvidence& evidence = entry.second;

    struct LineState {
      std::set<std::string> writers;
      std::uint64_t invalidations = 0;
      std::uint64_t shared_reads = 0;
      std::uint64_t writes = 0;
    };
    std::map<std::string, LineState> lines;
    bool any_line_identity = false;
    std::uint64_t total_invalidations = 0;
    std::uint64_t total_shared_reads = 0;
    std::set<std::string> region_writers;
    EvidenceGranularity finest = EvidenceGranularity::Unknown;
    Precision weakest_precision = Precision::ExactEvent;
    Provenance provenance = Provenance::Unknown;
    Reality reality = Reality::Real;
    bool have_sample = false;

    for (const OwnershipSample& sample : evidence.samples) {
      if (precision_rank(sample.precision) < precision_rank(policy.min_precision)) {
        continue;
      }
      have_sample = true;
      weakest_precision = weakest(weakest_precision, sample.precision);
      reality = combine_reality(reality, reality_of(sample.provenance));
      provenance = sample.provenance;
      if (granularity_rank(sample.granularity) > granularity_rank(finest)) {
        finest = sample.granularity;
      }
      std::string line_key = "unknown";
      if (sample.granularity == EvidenceGranularity::CacheLine && !sample.line_index.empty()) {
        any_line_identity = true;
        line_key = sample.line_index;
      }
      LineState& line = lines[line_key];
      if (sample.type == EventType::Invalidation || sample.type == EventType::RegionInvalidation) {
        ++line.invalidations;
        ++total_invalidations;
      }
      if (sample.type == EventType::SharedRead) {
        ++line.shared_reads;
        ++total_shared_reads;
      }
      if (sample.direction == AccessDirection::Write ||
          sample.direction == AccessDirection::ReadModifyWrite ||
          sample.type == EventType::RemoteWrite ||
          sample.type == EventType::WriteExclusiveTransition) {
        if (!sample.from.empty()) {
          line.writers.insert(sample.from.id.str());
          region_writers.insert(sample.from.id.str());
        }
        ++line.writes;
      }
    }
    if (!have_sample) {
      continue;
    }

    Finding finding;
    finding.kind = FindingKind::FalseSharingLike;
    finding.subject_kind = "region";
    finding.subject = entry.first;
    finding.granularity = finest;
    finding.precision = weakest_precision;
    finding.provenance = provenance;
    finding.reality = reality;
    finding.bindings = current_bindings(state);
    const auto region_it = state.regions.find(entry.first);
    if (region_it != state.regions.end()) {
      finding.bindings.region_generation = region_it->second.generation;
      finding.bindings.region_generation_bound = true;
    }

    std::uint64_t max_line_writers = 0;
    std::string hottest_line;
    for (const auto& line : lines) {
      if (line.second.writers.size() > max_line_writers) {
        max_line_writers = line.second.writers.size();
        hottest_line = line.first;
      }
    }

    add_metric(finding, "distinct_writers.region",
               static_cast<double>(region_writers.size()), "resources",
               static_cast<double>(policy.min_distinct_writers), true);
    add_metric(finding, "distinct_writers.line", static_cast<double>(max_line_writers),
               "resources", static_cast<double>(policy.min_distinct_writers), true);
    add_metric(finding, "invalidations", static_cast<double>(total_invalidations),
               "invalidations", static_cast<double>(policy.min_invalidations), true);
    add_metric(finding, "shared_reads", static_cast<double>(total_shared_reads), "reads",
               static_cast<double>(policy.min_shared_reads), true);

    const bool enough_writers = max_line_writers >= policy.min_distinct_writers ||
                                region_writers.size() >= policy.min_distinct_writers;
    const bool enough_invalidations = total_invalidations >= policy.min_invalidations;
    const bool enough_reads = total_shared_reads >= policy.min_shared_reads;

    switch (finest) {
      case EvidenceGranularity::CacheLine:
        if (any_line_identity && max_line_writers >= policy.min_distinct_writers &&
            enough_invalidations) {
          finding.contention = ContentionClass::CacheLineFalseSharingSupported;
          add_reason(finding, finding_reason::kDistinctWritersPerLine);
          add_reason(finding, finding_reason::kSharedReadWithInvalidations);
          add_metric(finding, "line.index", 0.0, "line:" + hottest_line, 0.0, false);
        } else if (!any_line_identity) {
          finding.contention = ContentionClass::InsufficientGranularity;
          add_reason(finding, finding_reason::kInsufficientGranularity);
          finding.precision = weakest(finding.precision, Precision::AggregatedCounter);
          add_missing(finding,
                      "cache-line identity (observation metadata key \"line.index\") is absent, "
                      "so line-level false sharing cannot be established");
        } else {
          finding.contention = ContentionClass::RegionContentionLikely;
          add_reason(finding, finding_reason::kRegionLevelContention);
        }
        break;
      case EvidenceGranularity::Page:
        finding.contention = ContentionClass::PageLevelContention;
        add_reason(finding, finding_reason::kRegionLevelContention);
        add_metric(finding, "page.level", 1.0, "page", 0.0, false);
        break;
      case EvidenceGranularity::Region:
        if (enough_writers && (enough_invalidations || enough_reads)) {
          finding.contention = ContentionClass::RegionContentionLikely;
          add_reason(finding, finding_reason::kRegionLevelContention);
        } else {
          finding.contention = ContentionClass::None;
        }
        break;
      default:
        finding.contention = ContentionClass::InsufficientGranularity;
        add_reason(finding, finding_reason::kInsufficientGranularity);
        add_missing(finding,
                    "evidence resolves only at " + std::string(to_string(finest)) +
                        " granularity; region or line contention cannot be established");
        break;
    }

    if (finding.contention == ContentionClass::None) {
      continue;
    }

    for (const OwnershipSample& sample : evidence.samples) {
      push_evidence(finding.evidence, evidence_ref(sample));
    }
    if (evidence.evicted != 0) {
      add_missing(finding, "region sample window evicted " + detail::format_u64(evidence.evicted) +
                               " older samples");
    }

    CostInputs inputs;
    inputs.invalidations = total_invalidations;
    inputs.precision = weakest_precision;
    finding.cost = estimate_cost(state.options.cost_model, inputs);
    sort_finding(finding);
    findings.push_back(std::move(finding));
  }
  finalize_findings(findings, policy.max_findings);
  return findings;
}

std::vector<Finding> analyze_invalidations(const State& state, const InvalidationPolicy& policy,
                                           std::vector<InvalidationRow>* rows) {
  std::vector<Finding> findings;
  struct Accum {
    InvalidationRow row;
    std::vector<Nanos> timestamps;
    std::string source;
    std::string target;
    std::string region;
    std::uint64_t region_generation = 0;
  };
  std::map<std::string, Accum> groups;
  std::uint64_t aggregate_counter_invalidations = 0;
  std::string key;
  key.reserve(256);

  for (const JournalEntry& entry : state.journal) {
    const Observation& observation = entry.observation;
    if (observation.counter_delta.has_value()) {
      if (observation.type == EventType::Invalidation ||
          observation.type == EventType::RegionInvalidation) {
        aggregate_counter_invalidations += *observation.counter_delta;
      }
      continue;
    }
    if (observation.type != EventType::Invalidation &&
        observation.type != EventType::RegionInvalidation) {
      continue;
    }
    const std::string_view source = observation.source.has_value()
                                       ? observation.source->id.view()
                                       : std::string_view("unknown");
    const std::string_view target = observation.target.has_value()
                                        ? observation.target->id.view()
                                        : std::string_view("unknown");
    const std::string_view region_name = observation.region.has_value()
                                             ? observation.region->view()
                                             : std::string_view("unknown");
    const std::uint64_t region_generation =
        observation.region_generation.has_value() ? observation.region_generation->value() : 0;
    key.clear();
    key.append(region_name);
    key.push_back('\x1f');
    key.append(source);
    key.push_back('\x1f');
    key.append(target);
    key.push_back('\x1f');
    key.append(detail::format_u64(region_generation));
    const bool first_seen = groups.find(key) == groups.end();
    Accum& accum = groups[key];
    if (first_seen) {
      accum.source.assign(source);
      accum.target.assign(target);
      accum.region.assign(region_name);
    }
    accum.region_generation = region_generation;
    accum.row.source = source;
    accum.row.target = target;
    if (observation.region.has_value()) {
      accum.row.region = *observation.region;
    }
    if (observation.region_generation.has_value()) {
      accum.row.region_generation = *observation.region_generation;
    }
    accum.row.state_before = observation.state_before;
    accum.row.state_after = observation.state_after;
    accum.row.granularity = observation.granularity;
    accum.row.precision = accum.row.individual_events == 0
                              ? observation.precision
                              : weakest(accum.row.precision, observation.precision);
    accum.row.provenance = observation.provenance;
    accum.row.reality = combine_reality(accum.row.reality, reality_of(observation.provenance));
    accum.row.bytes_invalidated += observation.bytes;
    ++accum.row.individual_events;
    accum.timestamps.push_back(observation.timestamp_ns);
  }

  for (auto& entry : groups) {
    Accum& accum = entry.second;
    std::sort(accum.timestamps.begin(), accum.timestamps.end());
    std::size_t burst_count = 0;
    std::uint64_t max_burst = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i < accum.timestamps.size(); ++i) {
      while (accum.timestamps[i] - accum.timestamps[start] > policy.burst_window_ns) {
        ++start;
      }
      const std::size_t size = i - start + 1;
      if (size >= policy.burst_threshold) {
        if (size == policy.burst_threshold || (i > 0 && accum.timestamps[i] != accum.timestamps[i - 1])) {
          ++burst_count;
        }
      }
      if (size > max_burst) {
        max_burst = size;
      }
    }
    accum.row.burst_count = burst_count;
    accum.row.max_burst_size = max_burst;

    CostInputs inputs;
    inputs.invalidations = accum.row.individual_events;
    inputs.bytes = accum.row.bytes_invalidated;
    inputs.precision = accum.row.precision;
    accum.row.cost = estimate_cost(state.options.cost_model, inputs);
    if (rows != nullptr && rows->size() < Limits::kMaxQueryResults) {
      rows->push_back(accum.row);
    }

    if (burst_count == 0) {
      continue;
    }
    Finding finding;
    finding.kind = FindingKind::InvalidationBurst;
    finding.subject_kind = "region";
    finding.subject = entry.second.region;
    finding.precision = accum.row.precision;
    finding.provenance = accum.row.provenance;
    finding.reality = accum.row.reality;
    finding.granularity = accum.row.granularity;
    finding.bindings = current_bindings(state);
    if (accum.region_generation != 0) {
      finding.bindings.region_generation = MemoryRegionGeneration{accum.region_generation};
      finding.bindings.region_generation_bound = true;
    }
    add_reason(finding, finding_reason::kInvalidationBurst);
    add_metric(finding, "bursts", static_cast<double>(burst_count), "bursts", 0.0, false);
    add_metric(finding, "max_burst_size", static_cast<double>(max_burst), "invalidations",
               static_cast<double>(policy.burst_threshold), true);
    add_metric(finding, "invalidations", static_cast<double>(accum.row.individual_events),
               "invalidations", static_cast<double>(policy.burst_threshold), true);
    add_metric(finding, "bytes_invalidated", static_cast<double>(accum.row.bytes_invalidated),
               "bytes", 0.0, false);
    add_participant(finding, accum.source);
    add_participant(finding, accum.target);
    finding.cost = accum.row.cost;
    sort_finding(finding);
    findings.push_back(std::move(finding));
  }

  if (aggregate_counter_invalidations != 0) {
    Finding finding;
    finding.kind = FindingKind::InvalidationBurst;
    finding.subject_kind = "system";
    finding.subject = "aggregate-invalidation-counters";
    finding.precision = Precision::AggregatedCounter;
    finding.provenance = Provenance::DerivedAggregation;
    finding.reality = Reality::Mixed;
    finding.granularity = EvidenceGranularity::Aggregate;
    finding.bindings = current_bindings(state);
    add_reason(finding, finding_reason::kInsufficientGranularity);
    add_missing(finding,
                "these invalidations were reported by aggregate counters and carry no source, "
                "target or region identity; they are not per-region evidence");
    add_metric(finding, "aggregate_counter_invalidations",
               static_cast<double>(aggregate_counter_invalidations), "invalidations", 0.0, false);
    sort_finding(finding);
    findings.push_back(std::move(finding));
  }

  if (rows != nullptr) {
    std::sort(rows->begin(), rows->end(), [](const InvalidationRow& a, const InvalidationRow& b) {
      if (a.region.str() != b.region.str()) {
        return a.region.str() < b.region.str();
      }
      if (a.source != b.source) {
        return a.source < b.source;
      }
      return a.target < b.target;
    });
  }
  finalize_findings(findings, policy.max_findings);
  return findings;
}

std::vector<Finding> analyze_remote_access(const State& state, const RemoteAccessPolicy& policy,
                                           std::vector<RemoteAccessRow>* rows) {
  std::vector<Finding> findings;
  std::map<std::string, RemoteAccessRow> groups;
  std::string key;
  key.reserve(256);

  for (const JournalEntry& entry : state.journal) {
    const Observation& observation = entry.observation;
    const bool remote_type = observation.type == EventType::RemoteRead ||
                             observation.type == EventType::RemoteWrite ||
                             observation.type == EventType::RemoteAtomic;
    const bool remote_locality =
        observation.locality_declared && is_remote_locality(observation.locality);
    if (!remote_type && !remote_locality) {
      continue;
    }
    const std::string_view source = observation.source.has_value()
                                       ? observation.source->id.view()
                                       : std::string_view("unknown");
    const std::string_view target = observation.target.has_value()
                                        ? observation.target->id.view()
                                        : std::string_view("unknown");
    const std::string_view region_name = observation.region.has_value()
                                             ? observation.region->view()
                                             : std::string_view("unknown");
    const std::uint64_t region_generation =
        observation.region_generation.has_value() ? observation.region_generation->value() : 0;
    key.clear();
    key.append(source);
    key.push_back('\x1f');
    key.append(target);
    key.push_back('\x1f');
    key.append(region_name);
    key.push_back('\x1f');
    key.append(detail::format_u64(region_generation));
    const bool first_seen = groups.find(key) == groups.end();
    RemoteAccessRow& row = groups[key];
    if (first_seen) {
      row.source.assign(source);
      row.target.assign(target);
    }
    if (observation.region.has_value()) {
      row.region = *observation.region;
    }
    if (observation.region_generation.has_value()) {
      row.region_generation = *observation.region_generation;
    }
    row.direction = observation.direction;
    row.locality = entry.locality;
    row.locality_established = entry.locality_established;
    row.topology_generation = observation.topology_generation;
    row.precision = row.accesses == 0 ? observation.precision
                                      : weakest(row.precision, observation.precision);
    row.provenance = observation.provenance;
    row.reality = combine_reality(row.reality, reality_of(observation.provenance));
    row.aggregate_only = observation.counter_delta.has_value();
    row.bytes += observation.bytes;
    ++row.accesses;
    if (observation.direction == AccessDirection::Write ||
        observation.direction == AccessDirection::ReadModifyWrite ||
        observation.type == EventType::RemoteWrite) {
      ++row.write_accesses;
    } else {
      ++row.read_accesses;
    }
  }

  std::uint64_t total_accesses = 0;
  for (auto& entry : groups) {
    RemoteAccessRow& row = entry.second;
    total_accesses += row.accesses;
    CostInputs inputs;
    inputs.bytes = row.bytes;
    inputs.remote_reads = row.read_accesses;
    inputs.remote_writes = row.write_accesses;
    inputs.locality = row.locality;
    inputs.locality_known = row.locality_established;
    inputs.precision = row.precision;
    row.cost = estimate_cost(state.options.cost_model, inputs);
  }

  RemoteAccessRow top_row;
  bool have_top = false;
  for (const auto& entry : groups) {
    if (!have_top || entry.second.accesses > top_row.accesses) {
      top_row = entry.second;
      have_top = true;
    }
  }
  if (have_top && total_accesses > 0) {
    const double concentration =
        static_cast<double>(top_row.accesses) / static_cast<double>(total_accesses);
    if (concentration >= policy.concentration_threshold) {
      Finding finding;
      finding.kind = FindingKind::RemoteAccessConcentration;
      finding.subject_kind = "source_target_pair";
      finding.subject = top_row.source + "->" + top_row.target;
      finding.precision = top_row.precision;
      finding.provenance = top_row.provenance;
      finding.reality = top_row.reality;
      finding.granularity = top_row.aggregate_only ? EvidenceGranularity::Aggregate
                                                   : EvidenceGranularity::Region;
      finding.bindings = current_bindings(state);
      finding.bindings.topology_generation = top_row.topology_generation;
      finding.locality = top_row.locality;
      finding.locality_established = top_row.locality_established;
      add_reason(finding, finding_reason::kSourceTargetConcentration);
      add_metric(finding, "pair.accesses", static_cast<double>(top_row.accesses), "accesses", 0.0,
                 false);
      add_metric(finding, "pair.share", concentration, "share", policy.concentration_threshold,
                 true);
      add_metric(finding, "system.remote_accesses", static_cast<double>(total_accesses),
                 "accesses", 0.0, false);
      add_metric(finding, "pair.bytes", static_cast<double>(top_row.bytes), "bytes", 0.0, false);
      if (top_row.aggregate_only) {
        add_missing(finding,
                    "evidence is counter-derived and resolves no region identity");
      }
      finding.cost = top_row.cost;
      sort_finding(finding);
      findings.push_back(std::move(finding));
    }
  }

  if (rows != nullptr) {
    for (const auto& entry : groups) {
      if (rows->size() >= policy.max_rows) {
        break;
      }
      rows->push_back(entry.second);
    }
  }
  finalize_findings(findings, policy.max_findings);
  return findings;
}

std::vector<Finding> analyze_operational(const State& state) {
  std::vector<Finding> findings;
  const GenerationBindings bindings = current_bindings(state);

  auto make = [&bindings](FindingKind kind, std::string subject_kind, std::string subject) {
    Finding finding;
    finding.kind = kind;
    finding.subject_kind = std::move(subject_kind);
    finding.subject = std::move(subject);
    finding.precision = Precision::ExactEvent;
    finding.provenance = Provenance::RuntimeInstrumentation;
    finding.reality = Reality::Real;
    finding.granularity = EvidenceGranularity::Device;
    finding.bindings = bindings;
    return finding;
  };

  // ---- Evidence loss ---------------------------------------------------
  const LossReport& loss = state.loss;
  if (loss.any()) {
    Finding finding = make(FindingKind::EvidenceLoss, "system", "ingestion");
    add_reason(finding, finding_reason::kDuplicateRejected);
    add_metric(finding, "rejected.total", static_cast<double>(loss.total_rejected()), "records",
               0.0, false);
    add_metric(finding, "rejected.observations",
               static_cast<double>(loss.rejected_observations), "records", 0.0, false);
    add_metric(finding, "rejected.malformed", static_cast<double>(loss.rejected_malformed),
               "records", 0.0, false);
    add_metric(finding, "rejected.unauthorized", static_cast<double>(loss.rejected_unauthorized),
               "records", 0.0, false);
    add_metric(finding, "rejected.overflow", static_cast<double>(loss.rejected_overflow),
               "records", 0.0, false);
    if (loss.missing_sequences != 0) {
      add_reason(finding, finding_reason::kMissingSequence);
      add_metric(finding, "missing_sequences", static_cast<double>(loss.missing_sequences),
                 "sequences", 0.0, false);
    }
    if (loss.rejected_conflicting_duplicates != 0) {
      add_metric(finding, "rejected.conflicting_duplicates",
                 static_cast<double>(loss.rejected_conflicting_duplicates), "records", 0.0,
                 false);
    }
    if (loss.rejected_stale_sequences != 0) {
      add_metric(finding, "rejected.stale_sequences",
                 static_cast<double>(loss.rejected_stale_sequences), "records", 0.0, false);
    }
    sort_finding(finding);
    findings.push_back(std::move(finding));
  }

  // ---- Capacity pressure ------------------------------------------------
  if (state.aggregates.refused_updates() != 0 || state.aggregates.saturations() != 0) {
    Finding finding = make(FindingKind::CapacityPressure, "system", "aggregation");
    if (state.aggregates.refused_updates() != 0) {
      add_reason(finding, finding_reason::kAggregateKeyPressure);
      add_metric(finding, "aggregate.refused_updates",
                 static_cast<double>(state.aggregates.refused_updates()), "updates",
                 static_cast<double>(Limits::kMaxAggregateKeys), true);
    }
    if (state.aggregates.saturations() != 0) {
      add_reason(finding, finding_reason::kAggregateSaturation);
      add_metric(finding, "aggregate.saturations",
                 static_cast<double>(state.aggregates.saturations()), "saturations", 0.0, false);
    }
    sort_finding(finding);
    findings.push_back(std::move(finding));
  }
  if (loss.dropped_journal_entries != 0 || loss.dropped_region_samples != 0) {
    Finding finding = make(FindingKind::CapacityPressure, "system", "evidence_retention");
    add_reason(finding, finding_reason::kJournalEviction);
    add_metric(finding, "dropped.journal_entries",
               static_cast<double>(loss.dropped_journal_entries), "records", 0.0, false);
    add_metric(finding, "dropped.region_samples",
               static_cast<double>(loss.dropped_region_samples), "records", 0.0, false);
    add_missing(finding, "pattern analyzers see only the retained window");
    sort_finding(finding);
    findings.push_back(std::move(finding));
  }

  // ---- Counter discontinuity -------------------------------------------
  for (const auto& entry : state.counters) {
    const CounterState& counter = entry.second;
    if (counter.resets == 0 && counter.wraps == 0) {
      continue;
    }
    Finding finding = make(FindingKind::CounterDiscontinuity, "counter", entry.first.counter);
    finding.provenance = counter.provenance;
    finding.reality = reality_of(counter.provenance);
    finding.granularity = counter.granularity;
    if (counter.resets != 0) {
      add_reason(finding, finding_reason::kCounterReset);
      add_metric(finding, "counter.resets", static_cast<double>(counter.resets), "resets", 0.0,
                 false);
    }
    if (counter.wraps != 0) {
      add_reason(finding, finding_reason::kCounterWrap);
      add_metric(finding, "counter.wraps", static_cast<double>(counter.wraps), "wraps", 0.0,
                 false);
    }
    add_metric(finding, "counter.accumulated", static_cast<double>(counter.accumulated), "units",
               0.0, false);
    add_metric(finding, "counter.samples", static_cast<double>(counter.samples), "samples", 0.0,
               false);
    add_metric(finding, "counter.generation",
               static_cast<double>(counter.generation.value()), "generation", 0.0, false);
    sort_finding(finding);
    findings.push_back(std::move(finding));
  }

  // ---- Sequence gaps and publisher loss --------------------------------
  for (const auto& entry : state.publishers) {
    const PublisherState& publisher = entry.second;
    if (publisher.view.sequences.missing_events != 0 ||
        publisher.view.sequences.sequence_gaps != 0) {
      Finding finding = make(FindingKind::SequenceGap, "publisher", entry.first);
      finding.provenance = publisher.view.provenance;
      finding.reality = publisher.view.reality;
      add_reason(finding, finding_reason::kMissingSequence);
      add_metric(finding, "sequence.gaps",
                 static_cast<double>(publisher.view.sequences.sequence_gaps), "gaps", 0.0, false);
      add_metric(finding, "sequence.missing",
                 static_cast<double>(publisher.view.sequences.missing_events), "sequences", 0.0,
                 false);
      add_metric(finding, "sequence.late",
                 static_cast<double>(publisher.view.sequences.late_events), "events", 0.0, false);
      add_missing(finding,
                  "events were never delivered; the runtime cannot say what they contained");
      sort_finding(finding);
      findings.push_back(std::move(finding));
    }
    if (publisher.view.fenced) {
      Finding finding = make(FindingKind::PublisherLoss, "publisher", entry.first);
      finding.provenance = publisher.view.provenance;
      finding.reality = publisher.view.reality;
      add_reason(finding, finding_reason::kPublisherFenced);
      add_metric(finding, "publisher.boot", static_cast<double>(publisher.view.boot.value()),
                 "boot", 0.0, false);
      add_metric(finding, "publisher.sequence_high_watermark",
                 static_cast<double>(publisher.view.sequences.high_watermark.value()), "sequence",
                 0.0, false);
      finding.ambiguity.push_back(std::string("fence_reason=") +
                                  std::string(to_string(publisher.view.fence_reason)));
      sort_finding(finding);
      findings.push_back(std::move(finding));
    }
  }

  // ---- Stale evidence summary ------------------------------------------
  if (!state.stale.empty()) {
    Finding finding = make(FindingKind::StaleEvidence, "system", "stale_evidence");
    finding.precision = Precision::AggregatedCounter;
    add_reason(finding, finding_reason::kStaleGenerations);
    add_metric(finding, "stale.records", static_cast<double>(state.stale.size()), "records", 0.0,
               false);
    std::map<std::string, std::uint64_t> by_kind;
    for (const StaleEvidenceRecord& record : state.stale) {
      ++by_kind[std::string(to_string(record.kind))];
    }
    for (const auto& kind : by_kind) {
      add_metric(finding, "stale." + kind.first, static_cast<double>(kind.second), "records", 0.0,
                 false);
    }
    sort_finding(finding);
    findings.push_back(std::move(finding));
  }

  // ---- Unsupported observability ---------------------------------------
  for (const auto& entry : state.capabilities) {
    if (entry.second.status != CapabilityStatus::Unsupported) {
      continue;
    }
    Finding finding = make(FindingKind::UnsupportedObservability, "capability", entry.first);
    finding.precision = Precision::Unknown;
    finding.provenance = Provenance::Unknown;
    finding.reality = Reality::Real;
    add_reason(finding, finding_reason::kNoCapability);
    finding.missing_evidence.push_back(entry.second.detail);
    finding.ambiguity.push_back(std::string("status=") +
                                std::string(to_string(entry.second.status)));
    sort_finding(finding);
    findings.push_back(std::move(finding));
  }

  return findings;
}

std::vector<Finding> analyze_all(const State& state, const FindingsOptions& options) {
  std::vector<Finding> findings;
  if (options.compute_hotspots) {
    const std::vector<Finding> hotspots = analyze_hotspots(state, state.options.hotspot_policy);
    findings.insert(findings.end(), hotspots.begin(), hotspots.end());
  }
  if (options.compute_ping_pong) {
    const std::vector<Finding> ping_pong =
        analyze_ping_pong(state, state.options.ping_pong_policy);
    findings.insert(findings.end(), ping_pong.begin(), ping_pong.end());
  }
  if (options.compute_false_sharing) {
    const std::vector<Finding> false_sharing =
        analyze_false_sharing(state, state.options.false_sharing_policy);
    findings.insert(findings.end(), false_sharing.begin(), false_sharing.end());
  }
  if (options.compute_invalidation) {
    const std::vector<Finding> invalidations =
        analyze_invalidations(state, state.options.invalidation_policy, nullptr);
    findings.insert(findings.end(), invalidations.begin(), invalidations.end());
  }
  if (options.compute_remote_access) {
    const std::vector<Finding> remote =
        analyze_remote_access(state, state.options.remote_access_policy, nullptr);
    findings.insert(findings.end(), remote.begin(), remote.end());
  }
  if (options.include_operational) {
    const std::vector<Finding> operational = analyze_operational(state);
    findings.insert(findings.end(), operational.begin(), operational.end());
  }
  finalize_findings(findings, options.max_findings);
  return findings;
}

std::size_t finalize_findings(std::vector<Finding>& findings, std::size_t max_findings) {
  std::stable_sort(findings.begin(), findings.end(), [](const Finding& a, const Finding& b) {
    return a.ordering_key() < b.ordering_key();
  });
  if (max_findings == 0) {
    const std::size_t dropped = findings.size();
    findings.clear();
    return dropped;
  }
  if (findings.size() <= max_findings) {
    return 0;
  }
  const std::size_t dropped = findings.size() - max_findings;
  findings.resize(max_findings);
  return dropped;
}

}  // namespace detail
}  // namespace sol::coherence