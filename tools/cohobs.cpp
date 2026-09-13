// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// cohobs -- read-only inspection.
//
// This tool never mutates coordinator state.  Administrative mutation lives in
// the separate cohobs-admin executable.

#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "cli_support.hpp"
#include "coherence/explanation.hpp"
#include "coherence/version.hpp"

namespace {

using sol::coherence::AggregateDimension;
using sol::coherence::AggregateKey;
using sol::coherence::Finding;
using sol::coherence::FindingsOptions;
using sol::coherence::Snapshot;

void print_usage() {
  std::cout <<
      "cohobs " << sol::coherence::library_version_string() << " -- read-only coherence inspection\n"
      "\n"
      "usage: cohobs (--endpoint host:port | --state <file>) <command> [arguments]\n"
      "\n"
      "commands:\n"
      "  summary                 epochs, publishers, regions, loss and capabilities\n"
      "  publishers              publisher authority, boot identity and sequence state\n"
      "  resources               nodes, processors, accelerators, memory and coherence domains\n"
      "  regions                 region identity, generation, domain and retirement\n"
      "  generations             every generation currently in force\n"
      "  evidence                current vs stale evidence accounting\n"
      "  events                  event totals per class\n"
      "  remote                  remote-access analysis rows\n"
      "  invalidations           invalidation analysis rows\n"
      "  transfers               ownership-transfer totals\n"
      "  locality                traffic by locality class\n"
      "  attribution <region>    attribution of one region\n"
      "  provenance              provenance and REAL/SYNTHETIC/MIXED breakdown\n"
      "  capabilities            REAL / SYNTHETIC / UNSUPPORTED capability classification\n"
      "  loss                    explicit evidence-loss accounting\n"
      "  findings                all findings\n"
      "  hotspots                hotspot findings\n"
      "  pingpong                ping-pong findings\n"
      "  falsesharing            false-sharing-like findings\n"
      "  explain                 every finding with its full explanation\n"
      "  json                    the whole snapshot as JSON\n";
}

FindingsOptions everything() {
  FindingsOptions options;
  options.compute_hotspots = true;
  options.compute_ping_pong = true;
  options.compute_false_sharing = true;
  options.compute_invalidation = true;
  options.compute_remote_access = true;
  options.include_operational = true;
  return options;
}

std::string dimension_value(const Snapshot& snapshot, AggregateDimension dimension,
                            const std::string& value) {
  const sol::coherence::AggregateValue* bucket =
      snapshot.aggregates().find(AggregateKey{dimension, value});
  return bucket == nullptr ? std::string("0") : std::to_string(bucket->observations);
}

int command_summary(const Snapshot& snapshot) {
  std::cout << "coordinator_epoch      " << snapshot.coordinator_epoch().value() << "\n";
  std::cout << "observation_epoch      " << snapshot.observation_epoch().value() << "\n";
  std::cout << "topology_generation    " << snapshot.topology_generation().value() << "\n";
  std::cout << "snapshot_generation    " << snapshot.generation().value() << "\n";
  std::cout << "reality                " << sol::coherence::to_string(snapshot.reality()) << "\n";
  std::cout << "publishers             " << snapshot.current_publisher_count() << " current of "
            << snapshot.publishers().size() << " registered\n";
  std::cout << "regions                " << snapshot.current_region_count() << " current of "
            << snapshot.regions().size() << " known\n";
  std::cout << "memory_domains         " << snapshot.memory_domains().size() << "\n";
  std::cout << "coherence_domains      " << snapshot.coherence_domains().size() << "\n";
  std::cout << "observations           "
            << snapshot.aggregates().observations_in(AggregateDimension::EventType) << "\n";
  std::cout << "bytes                  "
            << coherence_cli::bytes_text(snapshot.aggregates().bytes_in(AggregateDimension::Region))
            << "\n";
  std::cout << "stale_evidence_records " << snapshot.stale_evidence().size() << "\n";
  std::cout << "findings               " << snapshot.findings().size() << "\n";
  std::cout << "aggregate_buckets      " << snapshot.aggregates().buckets().size()
            << " carried of " << snapshot.aggregate_buckets_total() << " held ("
            << snapshot.aggregate_buckets_omitted() << " omitted over the wire bound)\n";
  std::cout << "rejected_evidence      " << snapshot.loss().loss.total_rejected() << "\n";
  std::cout << "missing_sequences      " << snapshot.loss().loss.missing_sequences << "\n";
  std::cout << "historical_observations " << snapshot.historical_observation_count()
            << " (source coordinator epoch "
            << snapshot.history_source_epoch().value() << "; never current)\n";
  return 0;
}

int command_publishers(const Snapshot& snapshot) {
  std::vector<std::vector<std::string>> rows;
  for (const sol::coherence::PublisherView& publisher : snapshot.publishers()) {
    rows.push_back({publisher.id.str(), std::to_string(publisher.boot.value()),
                    coherence_cli::bool_text(publisher.current),
                    coherence_cli::bool_text(publisher.fenced),
                    std::string(sol::coherence::to_string(publisher.fence_reason)),
                    std::string(sol::coherence::to_string(publisher.provenance)),
                    std::string(sol::coherence::to_string(publisher.reality)),
                    std::to_string(publisher.accepted_events),
                    std::to_string(publisher.sequences.high_watermark.value()),
                    std::to_string(publisher.sequences.missing_events),
                    std::to_string(publisher.sequences.duplicates),
                    coherence_cli::bool_text(publisher.loaded_from_state)});
  }
  std::cout << coherence_cli::render_table(
      {"publisher", "boot", "current", "fenced", "fence_reason", "provenance", "reality",
       "accepted", "high_watermark", "missing", "duplicates", "from_state"},
      rows);
  return 0;
}

int command_resources(const Snapshot& snapshot) {
  std::vector<std::vector<std::string>> rows;
  for (const sol::coherence::NodeRecord& node : snapshot.nodes()) {
    rows.push_back({"node", node.id.str(), "-", node.display_name, "-", "-"});
  }
  for (const sol::coherence::ProcessorRecord& processor : snapshot.processors()) {
    rows.push_back({"processor", processor.id.str(),
                    std::to_string(processor.generation.value()),
                    processor.local_memory_domain.str(),
                    coherence_cli::bool_text(processor.retired),
                    std::to_string(processor.logical_processor_count) + " logical"});
  }
  for (const sol::coherence::AcceleratorRecord& accelerator : snapshot.accelerators()) {
    rows.push_back({"accelerator", accelerator.id.str(),
                    std::to_string(accelerator.generation.value()),
                    accelerator.local_memory_domain.str(),
                    coherence_cli::bool_text(accelerator.retired), accelerator.vendor});
  }
  for (const sol::coherence::MemoryDomainRecord& domain : snapshot.memory_domains()) {
    rows.push_back({"memory_domain", domain.id.str(),
                    std::to_string(domain.generation.value()),
                    std::string(sol::coherence::to_string(domain.kind)),
                    coherence_cli::bool_text(domain.retired),
                    coherence_cli::bool_text(domain.coherent_with_host) + " coherent"});
  }
  for (const sol::coherence::CoherenceDomainRecord& domain : snapshot.coherence_domains()) {
    rows.push_back({"coherence_domain", domain.id.str(),
                    std::to_string(domain.generation.value()), domain.protocol_family,
                    coherence_cli::bool_text(domain.retired),
                    std::to_string(domain.members.size()) + " members"});
  }
  std::cout << coherence_cli::render_table(
      {"kind", "id", "generation", "detail", "retired", "extra"}, rows);
  return 0;
}

int command_regions(const Snapshot& snapshot) {
  std::vector<std::vector<std::string>> rows;
  for (const sol::coherence::RegionRecord& region : snapshot.regions()) {
    rows.push_back(
        {region.id.str(), std::to_string(region.generation.value()), region.memory_domain.str(),
         region.coherence_domain.str(), std::string(sol::coherence::to_string(region.sharing_scope)),
         coherence_cli::bytes_text(region.size_bytes), region.owner,
         coherence_cli::bool_text(region.retired),
         coherence_cli::bool_text(region.loaded_from_state)});
  }
  std::cout << coherence_cli::render_table(
      {"region", "generation", "memory_domain", "coherence_domain", "sharing", "size", "owner",
       "retired", "from_state"},
      rows);
  return 0;
}

int command_generations(const Snapshot& snapshot) {
  std::cout << "coordinator_epoch      " << snapshot.coordinator_epoch().value() << "\n";
  std::cout << "observation_epoch      " << snapshot.observation_epoch().value() << "\n";
  std::cout << "topology_generation    " << snapshot.topology_generation().value() << "\n";
  std::cout << "snapshot_generation    " << snapshot.generation().value() << "\n";
  std::cout << "\nper-resource generations:\n";
  std::vector<std::vector<std::string>> rows;
  for (const sol::coherence::ProcessorRecord& processor : snapshot.processors()) {
    rows.push_back({"processor", processor.id.str(),
                    std::to_string(processor.generation.value())});
  }
  for (const sol::coherence::AcceleratorRecord& accelerator : snapshot.accelerators()) {
    rows.push_back({"accelerator", accelerator.id.str(),
                    std::to_string(accelerator.generation.value())});
  }
  for (const sol::coherence::MemoryDomainRecord& domain : snapshot.memory_domains()) {
    rows.push_back({"memory_domain", domain.id.str(),
                    std::to_string(domain.generation.value())});
  }
  for (const sol::coherence::CoherenceDomainRecord& domain : snapshot.coherence_domains()) {
    rows.push_back({"coherence_domain", domain.id.str(),
                    std::to_string(domain.generation.value())});
  }
  for (const sol::coherence::RegionRecord& region : snapshot.regions()) {
    rows.push_back({"region", region.id.str(),
                    std::to_string(region.generation.value())});
  }
  std::cout << coherence_cli::render_table({"kind", "id", "generation"}, rows);
  return 0;
}

int command_evidence(const Snapshot& snapshot) {
  std::cout << "current observations   "
            << snapshot.aggregates().observations_in(AggregateDimension::EventType) << "\n";
  std::cout << "stale/unsupported      " << snapshot.stale_evidence().size() << "\n";
  std::cout << "historical (not current) " << snapshot.historical_observation_count() << "\n\n";
  std::vector<std::vector<std::string>> rows;
  for (const sol::coherence::StaleEvidenceRecord& record : snapshot.stale_evidence()) {
    rows.push_back({std::string(sol::coherence::to_string(record.kind)),
                    std::to_string(record.event_id.value()), record.publisher.str(),
                    std::to_string(record.publisher_boot.value()), record.region.str(),
                    std::string(sol::coherence::to_string(record.event_type)), record.detail});
  }
  std::cout << coherence_cli::render_table(
      {"kind", "event_id", "publisher", "boot", "region", "event_type", "detail"}, rows);
  return 0;
}

int command_events(const Snapshot& snapshot) {
  std::vector<std::vector<std::string>> rows;
  for (const auto& entry : snapshot.aggregates().buckets()) {
    if (entry.first.dimension != AggregateDimension::EventType) {
      continue;
    }
    rows.push_back({entry.first.value, std::to_string(entry.second.observations),
                    coherence_cli::bytes_text(entry.second.bytes),
                    std::to_string(entry.second.remote_accesses),
                    std::to_string(entry.second.invalidations),
                    std::to_string(entry.second.ownership_transfers),
                    coherence_cli::precision_breakdown(entry.second),
                    coherence_cli::reality_breakdown(entry.second)});
  }
  std::cout << coherence_cli::render_table(
      {"event_class", "observations", "bytes", "remote", "invalidations", "transfers",
       "precision", "reality"},
      rows);
  return 0;
}

int command_remote(sol::coherence::Observatory& observatory, bool live,
                   const sol::coherence::ObservationClient* client) {
  std::vector<sol::coherence::RemoteAccessRow> rows;
  if (live) {
    const sol::coherence::Result<std::vector<sol::coherence::RemoteAccessRow>> fetched =
        observatory.remote_access();
    if (!fetched.ok()) {
      std::cerr << "error: " << fetched.describe() << "\n";
      return 2;
    }
    rows = fetched.value();
  }
  (void)client;
  std::vector<std::vector<std::string>> table;
  for (const sol::coherence::RemoteAccessRow& row : rows) {
    table.push_back({row.source, row.target, row.region.str(),
                     std::to_string(row.region_generation.value()),
                     std::string(sol::coherence::to_string(row.direction)),
                     row.locality_established ? std::string(sol::coherence::to_string(row.locality))
                                              : std::string("UNKNOWN"),
                     std::to_string(row.accesses), coherence_cli::bytes_text(row.bytes),
                     std::to_string(row.read_accesses), std::to_string(row.write_accesses),
                     std::string(sol::coherence::to_string(row.cost.kind)),
                     std::to_string(row.topology_generation.value()),
                     std::string(sol::coherence::to_string(row.precision)),
                     coherence_cli::bool_text(row.aggregate_only)});
  }
  std::cout << coherence_cli::render_table(
      {"source", "target", "region", "region_gen", "direction", "locality", "accesses",
       "bytes", "reads", "writes", "cost_kind", "topology_gen", "precision", "aggregate_only"},
      table);
  return 0;
}

int command_invalidations(sol::coherence::Observatory& observatory) {
  const sol::coherence::Result<std::vector<sol::coherence::InvalidationRow>> fetched =
      observatory.invalidation_analysis();
  if (!fetched.ok()) {
    std::cerr << "error: " << fetched.describe() << "\n";
    return 2;
  }
  std::vector<std::vector<std::string>> table;
  for (const sol::coherence::InvalidationRow& row : fetched.value()) {
    table.push_back({row.source, row.target, row.region.str(),
                     std::to_string(row.individual_events),
                     std::to_string(row.aggregate_counter_invalidations),
                     std::to_string(row.burst_count), std::to_string(row.max_burst_size),
                     std::string(sol::coherence::to_string(row.granularity)),
                     std::string(sol::coherence::to_string(row.precision))});
  }
  std::cout << coherence_cli::render_table(
      {"source", "target", "region", "individual_events", "aggregate_counter_invalidations",
       "bursts", "max_burst", "granularity", "precision"},
      table);
  return 0;
}

int command_findings(const std::vector<Finding>& findings, sol::coherence::FindingKind filter,
                     bool has_filter, bool explain) {
  std::vector<Finding> selected;
  for (const Finding& finding : findings) {
    if (has_filter && finding.kind != filter) {
      continue;
    }
    selected.push_back(finding);
  }
  std::cout << coherence_cli::render_findings(selected, explain);
  return 0;
}

int command_attribution(coherence_cli::Reader& reader, const std::string& region_text) {
  const sol::coherence::Result<sol::coherence::MemoryRegionId> region =
      sol::coherence::MemoryRegionId::parse(region_text);
  if (!region.ok()) {
    std::cerr << "error: " << region.describe() << "\n";
    return 2;
  }
  const sol::coherence::Result<sol::coherence::AttributionResult> attribution =
      reader.attribution(region.value());
  if (!attribution.ok()) {
    std::cerr << "error: " << attribution.describe() << "\n";
    return 2;
  }
  std::cout << sol::coherence::render_text(sol::coherence::explain(attribution.value()));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> arguments;
  for (int i = 1; i < argc; ++i) {
    arguments.emplace_back(argv[i]);
  }
  if (arguments.empty() || arguments[0] == "--help" || arguments[0] == "-h") {
    print_usage();
    return arguments.empty() ? 1 : 0;
  }
  if (arguments[0] == "--version") {
    std::cout << sol::coherence::library_version_string() << " " << sol::coherence::build_identity()
              << "\n";
    return 0;
  }

  coherence_cli::Source source;
  std::vector<std::string> positional;
  const sol::coherence::Status parsed = coherence_cli::parse_source(arguments, &source, &positional);
  if (!parsed.ok()) {
    std::cerr << "error: " << parsed.describe() << "\n";
    print_usage();
    return 2;
  }
  if (positional.empty()) {
    std::cerr << "error: a command is required\n";
    print_usage();
    return 2;
  }
  const std::string command = positional[0];

  coherence_cli::Reader reader;
  const sol::coherence::Status opened = reader.open(source);
  if (!opened.ok()) {
    std::cerr << "error: " << opened.describe() << "\n";
    return 2;
  }

  const FindingsOptions options = everything();
  const sol::coherence::Result<sol::coherence::SnapshotPtr> snapshot = reader.snapshot(options);
  if (!snapshot.ok()) {
    std::cerr << "error: " << snapshot.describe() << "\n";
    return 2;
  }
  const Snapshot& view = *snapshot.value();

  if (command == "summary") return command_summary(view);
  if (command == "publishers") return command_publishers(view);
  if (command == "resources") return command_resources(view);
  if (command == "regions") return command_regions(view);
  if (command == "generations") return command_generations(view);
  if (command == "evidence") return command_evidence(view);
  if (command == "events") return command_events(view);
  if (command == "locality") {
    std::vector<std::vector<std::string>> rows;
    for (const auto& entry : view.aggregates().buckets()) {
      if (entry.first.dimension != AggregateDimension::Locality) {
        continue;
      }
      rows.push_back({entry.first.value, std::to_string(entry.second.observations),
                      coherence_cli::bytes_text(entry.second.bytes),
                      std::to_string(entry.second.remote_accesses),
                      std::string(sol::coherence::to_string(entry.second.reality()))});
    }
    std::cout << coherence_cli::render_table(
        {"locality", "observations", "bytes", "remote_accesses", "reality"}, rows);
    return 0;
  }
  if (command == "transfers") {
    std::vector<std::vector<std::string>> rows;
    for (const auto& entry : view.aggregates().buckets()) {
      if (entry.first.dimension != AggregateDimension::Region) {
        continue;
      }
      if (entry.second.ownership_transfers == 0) {
        continue;
      }
      rows.push_back({entry.first.value, std::to_string(entry.second.ownership_transfers),
                      std::to_string(entry.second.invalidations),
                      coherence_cli::bytes_text(entry.second.bytes)});
    }
    std::cout << coherence_cli::render_table(
        {"region", "ownership_transfers", "invalidations", "bytes"}, rows);
    return 0;
  }
  if (command == "provenance") {
    std::vector<std::vector<std::string>> rows;
    for (const auto& entry : view.aggregates().buckets()) {
      if (entry.first.dimension != AggregateDimension::Region) {
        continue;
      }
      std::string breakdown;
      for (std::size_t index = 0; index < entry.second.provenance_counts.size(); ++index) {
        if (entry.second.provenance_counts[index] == 0) {
          continue;
        }
        if (!breakdown.empty()) {
          breakdown.append(" ");
        }
        breakdown.append(sol::coherence::to_string(static_cast<sol::coherence::Provenance>(index)));
        breakdown.push_back('=');
        breakdown.append(std::to_string(entry.second.provenance_counts[index]));
      }
      rows.push_back({entry.first.value, breakdown.empty() ? "-" : breakdown,
                      std::string(sol::coherence::to_string(entry.second.reality()))});
    }
    std::cout << coherence_cli::render_table({"region", "provenance", "reality"}, rows);
    return 0;
  }
  if (command == "capabilities") {
    std::cout << coherence_cli::render_capabilities(view.capabilities());
    return 0;
  }
  if (command == "loss") {
    std::cout << sol::coherence::render_text(sol::coherence::explain_loss(view.loss().loss));
    return 0;
  }
  if (command == "findings") {
    return command_findings(view.findings(), sol::coherence::FindingKind::Hotspot, false, false);
  }
  if (command == "explain") {
    return command_findings(view.findings(), sol::coherence::FindingKind::Hotspot, false, true);
  }
  if (command == "hotspots") {
    return command_findings(view.findings(), sol::coherence::FindingKind::Hotspot, true, false);
  }
  if (command == "pingpong") {
    return command_findings(view.findings(), sol::coherence::FindingKind::PingPong, true, false);
  }
  if (command == "falsesharing") {
    return command_findings(view.findings(), sol::coherence::FindingKind::FalseSharingLike, true,
                            false);
  }
  if (command == "json") {
    std::cout << coherence_cli::render_snapshot_json(view);
    return 0;
  }
  if (command == "attribution") {
    if (positional.size() < 2) {
      std::cerr << "error: attribution requires a region identity\n";
      return 2;
    }
    return command_attribution(reader, positional[1]);
  }

  if (command == "remote" || command == "invalidations") {
    sol::coherence::Observatory* local = reader.local_observatory();
    if (local != nullptr) {
      if (command == "remote") return command_remote(*local, true, nullptr);
      return command_invalidations(*local);
    }
    // A remote coordinator exposes findings over the query protocol but not
    // per-pair tables; the tables require a durable state file.  Say so
    // instead of printing an empty table.
    std::cout << "note: per-pair tables require --state <file>; a live coordinator exposes "
                 "findings over the query protocol\n\n";
    const sol::coherence::FindingKind kind = command == "remote"
                                            ? sol::coherence::FindingKind::RemoteAccessConcentration
                                            : sol::coherence::FindingKind::InvalidationBurst;
    return command_findings(view.findings(), kind, true, false);
  }

  std::cerr << "error: unknown command '" << command << "'\n";
  print_usage();
  return 2;
}