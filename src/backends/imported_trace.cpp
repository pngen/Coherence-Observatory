// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Imported-trace backend.
//
// Canonical text format:
//
//   COBS-TRACE 1
//   # comments start with '#'
//   node <id> <display-name>
//   resource <KIND> <id> <generation>
//   region <id> <generation> <memory-domain> <size-bytes> <page-size> <SCOPE> <owner> <workload>
//   event <TYPE> <region|-> <region-generation|-> <source|-> <target|->
//         <DIRECTION> <STATE_BEFORE> <STATE_AFTER> <bytes> <lines> <PRECISION>
//         <GRANULARITY> <LOCALITY|-> <PROVENANCE> <timestamp-ns> [key=value ...]
//   counter <id> <TYPE> <KIND> <SCOPE> <width> <generation> <raw> <bytes-per-unit>
//           <PROVENANCE> <timestamp-ns> <source|-> [region|-]
//
// A field of "-" means "absent".  Imported evidence keeps
// Provenance::ImportedTrace unless the line names a different provenance, so it
// can never be reported as real hardware evidence.

#include "coherence/backends/imported_trace.hpp"

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "coherence/limits.hpp"
#include "text_util.hpp"

namespace sol::coherence {
namespace {

std::vector<std::string> tokenize(const std::string& line) {
  std::vector<std::string> tokens;
  std::istringstream stream(line);
  std::string token;
  while (stream >> token) {
    tokens.push_back(token);
  }
  return tokens;
}

bool parse_u64(const std::string& text, std::uint64_t* value) {
  if (text.empty()) {
    return false;
  }
  std::uint64_t result = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      return false;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (result > (~0ull - digit) / 10ull) {
      return false;
    }
    result = result * 10ull + digit;
  }
  *value = result;
  return true;
}

bool parse_i64(const std::string& text, std::int64_t* value) {
  if (!text.empty() && text[0] == '-') {
    std::uint64_t magnitude = 0;
    if (!parse_u64(text.substr(1), &magnitude)) {
      return false;
    }
    *value = -static_cast<std::int64_t>(magnitude);
    return true;
  }
  std::uint64_t magnitude = 0;
  if (!parse_u64(text, &magnitude)) {
    return false;
  }
  *value = static_cast<std::int64_t>(magnitude);
  return true;
}

/// Parses "<KIND>:<id>" or "-".
bool parse_resource(const std::string& text, std::optional<ResourceRef>* ref) {
  if (text == "-") {
    ref->reset();
    return true;
  }
  const std::size_t colon = text.find(':');
  if (colon == std::string::npos) {
    return false;
  }
  const Result<ResourceKind> kind = parse_resource_kind(text.substr(0, colon));
  if (!kind.ok()) {
    return false;
  }
  const std::string id = text.substr(colon + 1);
  const Result<ResourceId> parsed = ResourceId::parse(id);
  if (!parsed.ok()) {
    return false;
  }
  ResourceRef value;
  value.kind = kind.value();
  value.id = parsed.value();
  value.generation = 1;
  *ref = value;
  return true;
}

Status add_trailing_metadata(const std::vector<std::string>& tokens, std::size_t start,
                             BoundedMetadata* metadata) {
  for (std::size_t i = start; i < tokens.size(); ++i) {
    const std::size_t equals = tokens[i].find('=');
    if (equals == std::string::npos || equals == 0) {
      return fail(ErrorCode::InvalidArgument, "metadata token must be key=value", tokens[i]);
    }
    const Status added =
        metadata->add(tokens[i].substr(0, equals), tokens[i].substr(equals + 1));
    if (!added.ok()) {
      return added;
    }
  }
  return Status();
}

}  // namespace

Result<TraceImportReport> import_trace_text(std::string_view text, IngestionSink& sink,
                                            StructureRegistrar* registrar, bool strict) {
  TraceImportReport report;
  std::istringstream stream{std::string(text)};
  std::string line;
  bool header_seen = false;
  while (std::getline(stream, line)) {
    ++report.lines_read;
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty()) {
      continue;
    }
    if (line[0] == '#') {
      ++report.comments;
      continue;
    }
    if (!header_seen) {
      if (line != std::string(kTraceMagic)) {
        const Status status =
            fail(ErrorCode::InvalidArgument, "trace does not start with the canonical header",
                 detail::format_u64(report.lines_read));
        if (strict) {
          return Result<TraceImportReport>(status.error());
        }
        report.diagnostics.push_back(
            TraceImportDiagnostic{report.lines_read, status.describe()});
        return Result<TraceImportReport>(std::move(report));
      }
      header_seen = true;
      continue;
    }

    const std::vector<std::string> tokens = tokenize(line);
    if (tokens.empty()) {
      continue;
    }
    Status line_status;
    if (tokens[0] == "node") {
      if (tokens.size() < 2) {
        line_status = fail(ErrorCode::InvalidArgument, "node line needs an identity");
      } else if (registrar == nullptr) {
        ++report.descriptions_skipped;
      } else {
        NodeRecord record;
        const Result<NodeId> id = NodeId::parse(tokens[1]);
        if (!id.ok()) {
          line_status = Status(id.error());
        } else {
          record.id = id.value();
          record.display_name = tokens.size() > 2 ? tokens[2] : std::string();
          line_status = registrar->register_node(record);
        }
      }
    } else if (tokens[0] == "resource") {
      if (tokens.size() < 4) {
        line_status = fail(ErrorCode::InvalidArgument, "resource line needs 3 fields");
      } else if (registrar == nullptr) {
        ++report.descriptions_skipped;
      } else {
        const Result<ResourceKind> kind = parse_resource_kind(tokens[1]);
        std::uint64_t generation = 0;
        if (!kind.ok()) {
          line_status = Status(kind.error());
        } else if (!parse_u64(tokens[3], &generation)) {
          line_status = fail(ErrorCode::InvalidArgument, "resource generation is not numeric");
        } else {
          switch (kind.value()) {
            case ResourceKind::Processor: {
              ProcessorRecord record;
              const Result<ProcessorId> id = ProcessorId::parse(tokens[2]);
              if (!id.ok()) { line_status = Status(id.error()); break; }
              record.id = id.value();
              record.generation = DeviceGeneration{generation};
              record.node = NodeId{"node.host.0"};
              line_status = registrar->register_processor(record);
              break;
            }
            case ResourceKind::Accelerator: {
              AcceleratorRecord record;
              const Result<AcceleratorId> id = AcceleratorId::parse(tokens[2]);
              if (!id.ok()) { line_status = Status(id.error()); break; }
              record.id = id.value();
              record.generation = DeviceGeneration{generation};
              record.node = NodeId{"node.host.0"};
              record.vendor = tokens.size() > 4 ? tokens[4] : std::string("imported");
              line_status = registrar->register_accelerator(record);
              break;
            }
            case ResourceKind::MemoryDomain: {
              MemoryDomainRecord record;
              const Result<MemoryDomainId> id = MemoryDomainId::parse(tokens[2]);
              if (!id.ok()) { line_status = Status(id.error()); break; }
              record.id = id.value();
              record.generation = MemoryDomainGeneration{generation};
              record.node = NodeId{"node.host.0"};
              record.kind = MemoryDomainKind::Unknown;
              line_status = registrar->register_memory_domain(record);
              break;
            }
            case ResourceKind::CoherenceDomain: {
              CoherenceDomainRecord record;
              const Result<CoherenceDomainId> id = CoherenceDomainId::parse(tokens[2]);
              if (!id.ok()) { line_status = Status(id.error()); break; }
              record.id = id.value();
              record.generation = CoherenceDomainGeneration{generation};
              record.protocol_family = "imported";
              line_status = registrar->register_coherence_domain(record);
              break;
            }
            case ResourceKind::Node:
            case ResourceKind::Unknown:
              line_status = fail(ErrorCode::InvalidArgument,
                                 "resource line cannot describe this kind");
              break;
          }
          if (line_status.ok()) {
            ++report.resources_registered;
          }
        }
      }
    } else if (tokens[0] == "region") {
      // region <id> <generation> <memory-domain> <size> <page> <SCOPE> <owner> [workload]
      if (tokens.size() < 8) {
        line_status = fail(ErrorCode::InvalidArgument, "region line needs 8 fields");
      } else if (registrar == nullptr) {
        ++report.descriptions_skipped;
      } else {
        RegionRecord record;
        const Result<MemoryRegionId> id = MemoryRegionId::parse(tokens[1]);
        std::uint64_t generation = 0;
        std::uint64_t size = 0;
        std::uint64_t page = 0;
        const Result<SharingScope> scope = parse_sharing_scope(tokens[6]);
        if (!id.ok()) {
          line_status = Status(id.error());
        } else if (!parse_u64(tokens[2], &generation) || !parse_u64(tokens[4], &size) ||
                   !parse_u64(tokens[5], &page)) {
          line_status = fail(ErrorCode::InvalidArgument, "region numeric field is not numeric");
        } else if (!scope.ok()) {
          line_status = Status(scope.error());
        } else {
          record.id = id.value();
          record.generation = MemoryRegionGeneration{generation};
          record.memory_domain = MemoryDomainId{tokens[3]};
          record.memory_domain_generation = MemoryDomainGeneration{1};
          record.size_bytes = size;
          record.size_known = true;
          record.page_size_bytes = page;
          record.page_size_known = true;
          record.sharing_scope = scope.value();
          record.owner = tokens[7];
          record.workload = WorkloadId{tokens.size() > 8 ? tokens[8] : "workload.imported"};
          record.allocation_generation = 1;
          record.mapping_generation = 1;
          line_status = registrar->register_region(record);
          if (line_status.ok()) {
            ++report.regions_registered;
          }
        }
      }
    } else if (tokens[0] == "event") {
      // event <TYPE> <region|-> <region-generation|-> <source|-> <target|->
      //       <DIRECTION> <BEFORE> <AFTER> <bytes> <lines> <PRECISION>
      //       <GRANULARITY> <LOCALITY|-> <PROVENANCE> <timestamp> [k=v ...]
      if (tokens.size() < 16) {
        line_status = fail(ErrorCode::InvalidArgument, "event line needs 15 fields");
      } else {
        Observation observation;
        const Result<EventType> type = parse_event_type(tokens[1]);
        std::optional<ResourceRef> source;
        std::optional<ResourceRef> target;
        const Result<AccessDirection> direction = parse_access_direction(tokens[6]);
        const Result<CoherenceState> before = parse_coherence_state(tokens[7]);
        const Result<CoherenceState> after = parse_coherence_state(tokens[8]);
        std::uint64_t bytes = 0;
        std::uint64_t lines = 0;
        const Result<Precision> precision = parse_precision(tokens[11]);
        const Result<EvidenceGranularity> granularity = parse_evidence_granularity(tokens[12]);
        const Result<Provenance> provenance = parse_provenance(tokens[14]);
        std::uint64_t timestamp = 0;
        std::uint64_t region_generation = 0;
        if (!type.ok()) {
          line_status = Status(type.error());
        } else if (!parse_resource(tokens[4], &source) ||
                   !parse_resource(tokens[5], &target)) {
          line_status = fail(ErrorCode::InvalidArgument, "event resource field is malformed");
        } else if (!direction.ok()) {
          line_status = Status(direction.error());
        } else if (!before.ok() || !after.ok()) {
          line_status = fail(ErrorCode::InvalidArgument, "event coherence state is invalid");
        } else if (!parse_u64(tokens[9], &bytes) || !parse_u64(tokens[10], &lines)) {
          line_status = fail(ErrorCode::InvalidArgument, "event size field is not numeric");
        } else if (!precision.ok()) {
          line_status = Status(precision.error());
        } else if (!granularity.ok()) {
          line_status = Status(granularity.error());
        } else if (!provenance.ok()) {
          line_status = Status(provenance.error());
        } else if (!parse_u64(tokens[15], &timestamp)) {
          line_status = fail(ErrorCode::InvalidArgument, "event timestamp is not numeric");
        } else if (tokens[2] != "-" &&
                   (!parse_u64(tokens[3], &region_generation) || region_generation == 0)) {
          line_status = fail(ErrorCode::InvalidArgument,
                             "a region reference needs a non-zero generation");
        } else {
          observation.type = type.value();
          observation.source = source;
          observation.target = target;
          observation.direction = direction.value();
          observation.state_before = before.value();
          observation.state_after = after.value();
          observation.bytes = bytes;
          observation.lines = lines;
          observation.precision = precision.value();
          observation.granularity = granularity.value();
          observation.provenance = provenance.value();
          observation.timestamp_ns = static_cast<Nanos>(timestamp);
          if (tokens[2] != "-") {
            const Result<MemoryRegionId> region = MemoryRegionId::parse(tokens[2]);
            if (!region.ok()) {
              line_status = Status(region.error());
            } else {
              observation.region = region.value();
              observation.region_generation = MemoryRegionGeneration{region_generation};
            }
          }
          if (line_status.ok() && tokens[13] != "-") {
            const Result<Locality> locality = parse_locality(tokens[13]);
            if (!locality.ok()) {
              line_status = Status(locality.error());
            } else {
              observation.locality = locality.value();
              observation.locality_declared = true;
            }
          }
          if (line_status.ok() && tokens.size() > 16) {
            line_status = add_trailing_metadata(tokens, 16, &observation.metadata);
          }
          if (line_status.ok() && observation.type == EventType::UnknownCoherenceEvent &&
              !observation.metadata.contains("backend.event")) {
            line_status = fail(ErrorCode::InvalidArgument,
                               "unknown coherence event must name the backend event",
                               "backend.event");
          }
          if (line_status.ok()) {
            const Result<IngestionOutcome> outcome = sink.publish(std::move(observation));
            if (!outcome.ok()) {
              line_status = Status(outcome.error());
            } else if (outcome.value().disposition == IngestionDisposition::Rejected) {
              line_status = fail(ErrorCode::InvalidArgument, "coordinator rejected the event",
                                 outcome.value().detail);
            } else {
              ++report.observations_published;
            }
          }
        }
      }
    } else if (tokens[0] == "counter") {
      // counter <id> <TYPE> <KIND> <SCOPE> <width> <generation> <raw> <bytes-per-unit>
      //         <PROVENANCE> <timestamp> <source|-> [region|-]
      if (tokens.size() < 11) {
        line_status = fail(ErrorCode::InvalidArgument, "counter line needs 11 fields");
      } else {
        CounterPublication publication;
        const Result<CounterId> id = CounterId::parse(tokens[1]);
        const Result<EventType> type = parse_event_type(tokens[2]);
        const Result<CounterKind> kind = parse_counter_kind(tokens[3]);
        const Result<CounterScope> scope = parse_counter_scope(tokens[4]);
        const Result<Provenance> provenance = parse_provenance(tokens[9]);
        std::uint64_t width = 0;
        std::uint64_t generation = 0;
        std::uint64_t raw = 0;
        std::uint64_t scale = 0;
        std::uint64_t timestamp = 0;
        if (!id.ok()) {
          line_status = Status(id.error());
        } else if (!type.ok()) {
          line_status = Status(type.error());
        } else if (!kind.ok()) {
          line_status = Status(kind.error());
        } else if (!scope.ok()) {
          line_status = Status(scope.error());
        } else if (!parse_u64(tokens[5], &width) || !parse_u64(tokens[6], &generation) ||
                   !parse_u64(tokens[7], &raw) || !parse_u64(tokens[8], &scale) ||
                   !parse_u64(tokens[10], &timestamp)) {
          line_status = fail(ErrorCode::InvalidArgument, "counter numeric field is not numeric");
        } else if (!provenance.ok()) {
          line_status = Status(provenance.error());
        } else {
          publication.counter = id.value();
          publication.mapped_type = type.value();
          publication.kind = kind.value();
          publication.scope = scope.value();
          publication.width_bits = static_cast<std::uint32_t>(width);
          publication.generation = CounterGeneration{generation};
          publication.raw_value = raw;
          publication.bytes_per_unit = scale;
          publication.timestamp_ns = static_cast<Nanos>(timestamp);
          publication.provenance = provenance.value();
          publication.granularity = scope.value() == CounterScope::PerRegion
                                        ? EvidenceGranularity::Region
                                        : EvidenceGranularity::Device;
          std::optional<ResourceRef> source;
          if (parse_resource(tokens[11], &source) && source.has_value()) {
            publication.source = source;
          }
          std::optional<ResourceRef> region_ref;
          if (tokens.size() > 12 && parse_resource(tokens[12], &region_ref) &&
              region_ref.has_value()) {
            const Result<MemoryRegionId> region = MemoryRegionId::parse(region_ref->id.str());
            if (!region.ok()) {
              line_status = Status(region.error());
            } else {
              publication.region = region.value();
              publication.region_generation = MemoryRegionGeneration{1};
            }
          }
          const Result<CounterOutcome> outcome = sink.publish_counter(std::move(publication));
          if (!outcome.ok()) {
            line_status = Status(outcome.error());
          } else {
            ++report.counters_published;
          }
        }
      }
    } else {
      line_status = fail(ErrorCode::InvalidArgument, "unknown trace directive", tokens[0]);
    }

    if (!line_status.ok()) {
      ++report.rejected;
      report.diagnostics.push_back(
          TraceImportDiagnostic{report.lines_read, line_status.describe()});
      if (strict) {
        return fail_as<TraceImportReport>(line_status.code(), line_status.describe(),
                                          detail::format_u64(report.lines_read));
      }
    }
  }
  if (!header_seen) {
    return fail_as<TraceImportReport>(ErrorCode::InvalidArgument, "trace has no header");
  }
  return Result<TraceImportReport>(std::move(report));
}

Result<TraceImportReport> import_trace_file(const std::filesystem::path& path,
                                            IngestionSink& sink,
                                            StructureRegistrar* registrar, bool strict) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    return fail_as<TraceImportReport>(ErrorCode::NotFound, "cannot open trace file",
                                      path.string());
  }
  std::string content;
  std::string line;
  while (std::getline(stream, line)) {
    content.append(line);
    content.push_back('\n');
    if (content.size() > Limits::kMaxPersistenceBytes) {
      return fail_as<TraceImportReport>(ErrorCode::TooLarge, "trace file exceeds the bound",
                                        path.string());
    }
  }
  return import_trace_text(content, sink, registrar, strict);
}

std::string render_trace(const std::vector<Observation>& observations) {
  std::string out(kTraceMagic);
  out.push_back('\n');
  for (const Observation& observation : observations) {
    out.append("event ");
    out.append(to_string(observation.type));
    out.push_back(' ');
    out.append(observation.region.has_value() ? observation.region->str() : "-");
    out.push_back(' ');
    out.append(observation.region_generation.has_value()
                   ? detail::format_u64(observation.region_generation->value())
                   : "-");
    out.push_back(' ');
    out.append(observation.source.has_value()
                   ? std::string(to_string(observation.source->kind)) + ":" +
                         observation.source->id.str()
                   : "-");
    out.push_back(' ');
    out.append(observation.target.has_value()
                   ? std::string(to_string(observation.target->kind)) + ":" +
                         observation.target->id.str()
                   : "-");
    out.push_back(' ');
    out.append(to_string(observation.direction));
    out.push_back(' ');
    out.append(to_string(observation.state_before));
    out.push_back(' ');
    out.append(to_string(observation.state_after));
    out.push_back(' ');
    out.append(detail::format_u64(observation.bytes));
    out.push_back(' ');
    out.append(detail::format_u64(observation.lines));
    out.push_back(' ');
    out.append(to_string(observation.precision));
    out.push_back(' ');
    out.append(to_string(observation.granularity));
    out.push_back(' ');
    out.append(observation.locality_declared ? std::string(to_string(observation.locality))
                                             : std::string("-"));
    out.push_back(' ');
    out.append(to_string(observation.provenance));
    out.push_back(' ');
    out.append(detail::format_i64(observation.timestamp_ns));
    for (const auto& entry : observation.metadata.sorted_entries()) {
      out.push_back(' ');
      out.append(entry.first);
      out.push_back('=');
      out.append(entry.second);
    }
    out.push_back('\n');
  }
  return out;
}

// ---- ImportedTraceBackend ----------------------------------------------

struct ImportedTraceBackend::Impl {
  TraceImportConfig config;
  TraceImportReport report;
  bool started = false;
  bool imported = false;
};

ImportedTraceBackend::ImportedTraceBackend(TraceImportConfig config)
    : impl_(std::make_unique<Impl>()) {
  impl_->config = std::move(config);
}

ImportedTraceBackend::~ImportedTraceBackend() { stop(); }

std::string_view ImportedTraceBackend::name() const noexcept { return "imported-trace"; }

std::vector<Capability> ImportedTraceBackend::capabilities() const {
  std::vector<Capability> capabilities;
  Capability capability;
  capability.key = "trace.import";
  capability.status = CapabilityStatus::Synthetic;
  capability.detail =
      "evidence imported from a trace file keeps ImportedTrace provenance and is never "
      "reported as real hardware evidence";
  capability.mechanism = "COBS-TRACE text format";
  capabilities.push_back(std::move(capability));
  return capabilities;
}

Status ImportedTraceBackend::start(const CollectorContext& context) {
  if (context.sink == nullptr) {
    return fail(ErrorCode::InvalidArgument, "trace backend requires an ingestion sink");
  }
  if (impl_->config.path.empty()) {
    return fail(ErrorCode::InvalidArgument, "trace path is empty");
  }
  const Result<TraceImportReport> report =
      import_trace_file(impl_->config.path, *context.sink, context.registrar,
                        impl_->config.strict);
  if (!report.ok()) {
    return Status(report.error());
  }
  impl_->report = report.value();
  impl_->imported = true;
  impl_->started = true;
  return Status();
}

Status ImportedTraceBackend::poll(const CollectorContext& context) {
  (void)context;
  if (!impl_->started) {
    return fail(ErrorCode::Busy, "trace backend is not started");
  }
  return Status();
}

Status ImportedTraceBackend::stop() {
  impl_->started = false;
  return Status();
}

const TraceImportReport& ImportedTraceBackend::report() const noexcept {
  return impl_->report;
}

}  // namespace sol::coherence