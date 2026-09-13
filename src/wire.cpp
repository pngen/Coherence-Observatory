// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "detail/wire.hpp"

#include <string>
#include <vector>

#include "detail/state.hpp"
#include "text_util.hpp"

namespace sol::coherence::detail {
namespace {

constexpr std::size_t kMaxTextShort = Limits::kMaxNameLength;
/// Aggregate buckets carried by one snapshot response.  A coordinator holding
/// more reports the omission explicitly rather than truncating silently.
constexpr std::uint32_t kMaxWireAggregateBuckets = 2048;
constexpr std::size_t kMaxTextAnnotation = Limits::kMaxRegionAnnotationLength;

Status require_enum(PayloadReader& reader, std::uint8_t* out, std::uint8_t exclusive_max,
                    const char* what) {
  const Status status = reader.u8(out);
  if (!status.ok()) {
    return status;
  }
  if (*out >= exclusive_max) {
    return fail(ErrorCode::InvalidArgument, std::string(what) + " out of range",
                format_u64(*out));
  }
  return Status();
}

Status write_resource_ref(PayloadWriter& writer, const ResourceRef& ref) {
  writer.u8(static_cast<std::uint8_t>(ref.kind));
  const Status status = writer.text(ref.id.view(), kMaxTextShort);
  if (!status.ok()) {
    return status;
  }
  writer.u64(ref.generation);
  return Status();
}

Status read_resource_ref(PayloadReader& reader, ResourceRef* ref) {
  std::uint8_t kind = 0;
  Status status = require_enum(reader, &kind, 6, "resource kind");
  if (!status.ok()) {
    return status;
  }
  std::string id;
  status = reader.text(&id, kMaxTextShort);
  if (!status.ok()) {
    return status;
  }
  std::uint64_t generation = 0;
  status = reader.u64(&generation);
  if (!status.ok()) {
    return status;
  }
  ref->kind = static_cast<ResourceKind>(kind);
  const Result<ResourceId> parsed = ResourceId::parse(id);
  if (!parsed.ok()) {
    return Status(parsed.error());
  }
  ref->id = parsed.value();
  ref->generation = generation;
  return Status();
}

template <class Id>
Status write_name_id(PayloadWriter& writer, const Id& id) {
  return writer.text(id.view(), kMaxTextShort);
}

template <class Id>
Status read_name_id(PayloadReader& reader, Id* id) {
  std::string text;
  const Status status = reader.text(&text, kMaxTextShort);
  if (!status.ok()) {
    return status;
  }
  if (text.empty()) {
    *id = Id{};
    return Status();
  }
  const Result<Id> parsed = Id::parse(text);
  if (!parsed.ok()) {
    return Status(parsed.error());
  }
  *id = parsed.value();
  return Status();
}

Status write_optional_resource(PayloadWriter& writer,
                               const std::optional<ResourceRef>& ref) {
  writer.u8(ref.has_value() ? std::uint8_t{1} : std::uint8_t{0});
  if (!ref.has_value()) {
    return Status();
  }
  return write_resource_ref(writer, *ref);
}

Status read_optional_resource(PayloadReader& reader, std::optional<ResourceRef>* ref) {
  std::uint8_t present = 0;
  Status status = reader.u8(&present);
  if (!status.ok()) {
    return status;
  }
  if (present == 0) {
    ref->reset();
    return Status();
  }
  ResourceRef value;
  status = read_resource_ref(reader, &value);
  if (!status.ok()) {
    return status;
  }
  *ref = value;
  return Status();
}

void write_metadata(PayloadWriter& writer, const BoundedMetadata& metadata) {
  const std::vector<std::pair<std::string, std::string>> entries = metadata.sorted_entries();
  writer.u32(static_cast<std::uint32_t>(entries.size()));
  for (const auto& entry : entries) {
    writer.text(entry.first, Limits::kMaxMetadataKeyLength);
    writer.text(entry.second, Limits::kMaxMetadataValueLength);
  }
}

Status read_metadata(PayloadReader& reader, BoundedMetadata* metadata) {
  std::uint32_t count = 0;
  Status status = reader.u32(&count);
  if (!status.ok()) {
    return status;
  }
  if (count > Limits::kMaxMetadataEntries) {
    return fail(ErrorCode::TooMany, "metadata entry count exceeds the bound",
                format_u64(count));
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string key;
    std::string value;
    status = reader.text(&key, Limits::kMaxMetadataKeyLength);
    if (!status.ok()) {
      return status;
    }
    status = reader.text(&value, Limits::kMaxMetadataValueLength);
    if (!status.ok()) {
      return status;
    }
    status = metadata->add(key, value);
    if (!status.ok()) {
      return status;
    }
  }
  return Status();
}

void write_evidence(PayloadWriter& writer, const std::vector<EvidenceRef>& evidence) {
  const std::size_t count =
      evidence.size() > Limits::kMaxEvidenceRefs ? Limits::kMaxEvidenceRefs : evidence.size();
  writer.u32(static_cast<std::uint32_t>(count));
  for (std::size_t i = 0; i < count; ++i) {
    const EvidenceRef& ref = evidence[i];
    writer.u64(ref.event_id.value());
    writer.text(ref.publisher.view(), kMaxTextShort);
    writer.u64(ref.publisher_boot.value());
    writer.u64(ref.sequence.value());
    writer.u8(static_cast<std::uint8_t>(ref.event_type));
    writer.u8(static_cast<std::uint8_t>(ref.precision));
    writer.u8(static_cast<std::uint8_t>(ref.provenance));
  }
}

Status read_evidence(PayloadReader& reader, std::vector<EvidenceRef>* evidence) {
  std::uint32_t count = 0;
  Status status = reader.u32(&count);
  if (!status.ok()) {
    return status;
  }
  if (count > Limits::kMaxEvidenceRefs) {
    return fail(ErrorCode::TooMany, "evidence reference count exceeds the bound");
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    EvidenceRef ref;
    std::uint64_t event_id = 0;
    std::uint64_t boot = 0;
    std::uint64_t sequence = 0;
    std::uint8_t type = 0;
    std::uint8_t precision = 0;
    std::uint8_t provenance = 0;
    status = reader.u64(&event_id);
    if (!status.ok()) return status;
    status = read_name_id(reader, &ref.publisher);
    if (!status.ok()) return status;
    status = reader.u64(&boot);
    if (!status.ok()) return status;
    status = reader.u64(&sequence);
    if (!status.ok()) return status;
    status = require_enum(reader, &type, kEventTypeCount, "evidence event type");
    if (!status.ok()) return status;
    status = require_enum(reader, &precision, 7, "evidence precision");
    if (!status.ok()) return status;
    status = require_enum(reader, &provenance, 10, "evidence provenance");
    if (!status.ok()) return status;
    ref.event_id = CoherenceEventId{event_id};
    ref.publisher_boot = PublisherBootId{boot};
    ref.sequence = EventSequence{sequence};
    ref.event_type = static_cast<EventType>(type);
    ref.precision = static_cast<Precision>(precision);
    ref.provenance = static_cast<Provenance>(provenance);
    evidence->push_back(std::move(ref));
  }
  return Status();
}

Status write_string_list(PayloadWriter& writer, const std::vector<std::string>& values,
                         std::size_t max_count, std::size_t max_length) {
  const std::size_t count = values.size() > max_count ? max_count : values.size();
  writer.u32(static_cast<std::uint32_t>(count));
  for (std::size_t i = 0; i < count; ++i) {
    const Status status = writer.text(values[i], max_length);
    if (!status.ok()) {
      return status;
    }
  }
  return Status();
}

Status read_string_list(PayloadReader& reader, std::vector<std::string>* values,
                        std::size_t max_count, std::size_t max_length) {
  std::uint32_t count = 0;
  Status status = reader.u32(&count);
  if (!status.ok()) {
    return status;
  }
  if (count > max_count) {
    return fail(ErrorCode::TooMany, "list count exceeds the bound", format_u64(count));
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    std::string value;
    status = reader.text(&value, max_length);
    if (!status.ok()) {
      return status;
    }
    values->push_back(std::move(value));
  }
  return Status();
}

void write_cost(PayloadWriter& writer, const CostEstimate& cost) {
  writer.u8(static_cast<std::uint8_t>(cost.kind));
  writer.f64(cost.latency_ns);
  writer.u64(cost.bytes);
  writer.f64(cost.bandwidth_bytes_per_ns);
  writer.u64(cost.remote_accesses);
  writer.u64(cost.ownership_transfers);
  writer.u64(cost.invalidations);
  writer.u64(cost.retries);
  writer.u64(cost.stall_cycles);
  writer.text(cost.model_id, 96);
  writer.u32(cost.model_version);
  writer.u8(static_cast<std::uint8_t>(cost.precision));
  writer.u8(cost.lower_bound);
  const std::size_t terms = cost.terms.size() > 32 ? 32 : cost.terms.size();
  writer.u32(static_cast<std::uint32_t>(terms));
  for (std::size_t i = 0; i < terms; ++i) {
    const CostTerm& term = cost.terms[i];
    writer.text(term.name, kMaxTextShort);
    writer.u8(static_cast<std::uint8_t>(term.dimension));
    writer.f64(term.value);
    writer.u8(static_cast<std::uint8_t>(term.kind));
    writer.f64(term.coefficient);
    writer.f64(term.quantity);
    writer.text(term.unit, 24);
  }
}

Status read_cost(PayloadReader& reader, CostEstimate* cost) {
  std::uint8_t kind = 0;
  std::uint8_t precision = 0;
  std::uint8_t lower_bound = 0;
  Status status = require_enum(reader, &kind, 4, "cost kind");
  if (!status.ok()) return status;
  status = reader.f64(&cost->latency_ns);
  if (!status.ok()) return status;
  status = reader.u64(&cost->bytes);
  if (!status.ok()) return status;
  status = reader.f64(&cost->bandwidth_bytes_per_ns);
  if (!status.ok()) return status;
  status = reader.u64(&cost->remote_accesses);
  if (!status.ok()) return status;
  status = reader.u64(&cost->ownership_transfers);
  if (!status.ok()) return status;
  status = reader.u64(&cost->invalidations);
  if (!status.ok()) return status;
  status = reader.u64(&cost->retries);
  if (!status.ok()) return status;
  status = reader.u64(&cost->stall_cycles);
  if (!status.ok()) return status;
  status = reader.text(&cost->model_id, 96);
  if (!status.ok()) return status;
  status = reader.u32(&cost->model_version);
  if (!status.ok()) return status;
  status = require_enum(reader, &precision, 7, "cost precision");
  if (!status.ok()) return status;
  status = reader.u8(&lower_bound);
  if (!status.ok()) return status;
  std::uint32_t term_count = 0;
  status = reader.u32(&term_count);
  if (!status.ok()) return status;
  if (term_count > 32) {
    return fail(ErrorCode::TooMany, "cost term count exceeds the bound");
  }
  cost->kind = static_cast<CostKind>(kind);
  cost->precision = static_cast<Precision>(precision);
  cost->lower_bound = lower_bound != 0;
  for (std::uint32_t i = 0; i < term_count; ++i) {
    CostTerm term;
    std::uint8_t dimension = 0;
    std::uint8_t term_kind = 0;
    status = reader.text(&term.name, kMaxTextShort);
    if (!status.ok()) return status;
    status = require_enum(reader, &dimension, 14, "cost dimension");
    if (!status.ok()) return status;
    status = reader.f64(&term.value);
    if (!status.ok()) return status;
    status = require_enum(reader, &term_kind, 4, "cost term kind");
    if (!status.ok()) return status;
    status = reader.f64(&term.coefficient);
    if (!status.ok()) return status;
    status = reader.f64(&term.quantity);
    if (!status.ok()) return status;
    status = reader.text(&term.unit, 24);
    if (!status.ok()) return status;
    term.dimension = static_cast<CostDimension>(dimension);
    term.kind = static_cast<CostKind>(term_kind);
    cost->terms.push_back(std::move(term));
  }
  return Status();
}

void write_bindings(PayloadWriter& writer, const GenerationBindings& bindings) {
  writer.u64(bindings.coordinator_epoch.value());
  writer.u64(bindings.observation_epoch.value());
  writer.u64(bindings.topology_generation.value());
  writer.u64(bindings.evidence_generation.value());
  writer.u64(bindings.source_device_generation.value());
  writer.u64(bindings.target_device_generation.value());
  writer.u64(bindings.region_generation.value());
  writer.u8(bindings.region_generation_bound);
  writer.u64(bindings.source_domain_generation.value());
  writer.u64(bindings.target_domain_generation.value());
}

Status read_bindings(PayloadReader& reader, GenerationBindings* bindings) {
  std::uint64_t coordinator = 0;
  std::uint64_t observation = 0;
  std::uint64_t topology = 0;
  std::uint64_t evidence = 0;
  std::uint64_t source_device = 0;
  std::uint64_t target_device = 0;
  std::uint64_t region = 0;
  std::uint64_t source_domain = 0;
  std::uint64_t target_domain = 0;
  std::uint8_t region_bound = 0;
  Status status = reader.u64(&coordinator);
  if (!status.ok()) return status;
  status = reader.u64(&observation);
  if (!status.ok()) return status;
  status = reader.u64(&topology);
  if (!status.ok()) return status;
  status = reader.u64(&evidence);
  if (!status.ok()) return status;
  status = reader.u64(&source_device);
  if (!status.ok()) return status;
  status = reader.u64(&target_device);
  if (!status.ok()) return status;
  status = reader.u64(&region);
  if (!status.ok()) return status;
  status = reader.u8(&region_bound);
  if (!status.ok()) return status;
  status = reader.u64(&source_domain);
  if (!status.ok()) return status;
  status = reader.u64(&target_domain);
  if (!status.ok()) return status;
  bindings->coordinator_epoch = CoordinatorEpoch{coordinator};
  bindings->observation_epoch = ObservationEpoch{observation};
  bindings->topology_generation = TopologyGeneration{topology};
  bindings->evidence_generation = EvidenceGeneration{evidence};
  bindings->source_device_generation = DeviceGeneration{source_device};
  bindings->target_device_generation = DeviceGeneration{target_device};
  bindings->region_generation = MemoryRegionGeneration{region};
  bindings->region_generation_bound = region_bound != 0;
  bindings->source_domain_generation = MemoryDomainGeneration{source_domain};
  bindings->target_domain_generation = MemoryDomainGeneration{target_domain};
  return Status();
}

}  // namespace

Status write_observation(PayloadWriter& writer, const Observation& observation) {
  writer.u64(observation.event_id.value());
  writer.u8(static_cast<std::uint8_t>(observation.type));
  Status status = write_name_id(writer, observation.source_publisher);
  if (!status.ok()) return status;
  writer.u64(observation.publisher_boot.value());
  writer.u64(observation.coordinator_epoch.value());
  writer.u64(observation.sampling_epoch.value());
  writer.u64(observation.evidence_generation.value());
  writer.u64(observation.sequence.value());
  writer.i64(observation.timestamp_ns);
  status = write_optional_resource(writer, observation.source);
  if (!status.ok()) return status;
  status = write_optional_resource(writer, observation.target);
  if (!status.ok()) return status;
  writer.u8(observation.region.has_value());
  if (observation.region.has_value()) {
    status = write_name_id(writer, *observation.region);
    if (!status.ok()) return status;
  }
  writer.u64(observation.region_generation.value_or(MemoryRegionGeneration{}).value());
  writer.u8(observation.coherence_domain.has_value());
  if (observation.coherence_domain.has_value()) {
    status = write_name_id(writer, *observation.coherence_domain);
    if (!status.ok()) return status;
  }
  writer.u64(
      observation.coherence_domain_generation.value_or(CoherenceDomainGeneration{}).value());
  writer.u64(observation.topology_generation.value());
  writer.u8(observation.workload.has_value());
  if (observation.workload.has_value()) {
    status = write_name_id(writer, *observation.workload);
    if (!status.ok()) return status;
  }
  writer.u8(observation.process.has_value());
  if (observation.process.has_value()) {
    status = write_name_id(writer, *observation.process);
    if (!status.ok()) return status;
  }
  writer.u8(static_cast<std::uint8_t>(observation.direction));
  writer.u8(static_cast<std::uint8_t>(observation.state_before));
  writer.u8(static_cast<std::uint8_t>(observation.state_after));
  writer.u8(observation.counter_delta.has_value());
  writer.u64(observation.counter_delta.value_or(0));
  writer.u64(observation.counter_generation.value());
  writer.u64(observation.bytes);
  writer.u64(observation.lines);
  writer.u64(observation.pages);
  writer.u64(observation.region_count);
  writer.i64(observation.measured_duration_ns);
  writer.u8(static_cast<std::uint8_t>(observation.locality));
  writer.u8(observation.locality_declared);
  writer.u8(static_cast<std::uint8_t>(observation.provenance));
  writer.u8(static_cast<std::uint8_t>(observation.precision));
  writer.u8(static_cast<std::uint8_t>(observation.granularity));
  write_metadata(writer, observation.metadata);
  return Status();
}

Result<Observation> read_observation(PayloadReader& reader) {
  Observation observation;
  std::uint64_t event_id = 0;
  std::uint64_t boot = 0;
  std::uint64_t coordinator = 0;
  std::uint64_t sampling = 0;
  std::uint64_t evidence = 0;
  std::uint64_t sequence = 0;
  std::int64_t timestamp = 0;
  std::uint8_t type = 0;
  std::uint8_t direction = 0;
  std::uint8_t state_before = 0;
  std::uint8_t state_after = 0;
  std::uint8_t has_counter = 0;
  std::uint64_t counter_delta = 0;
  std::uint64_t counter_generation = 0;
  std::int64_t measured = -1;
  std::uint8_t locality = 0;
  std::uint8_t locality_declared = 0;
  std::uint8_t provenance = 0;
  std::uint8_t precision = 0;
  std::uint8_t granularity = 0;
  std::uint8_t has_region = 0;
  std::uint8_t has_domain = 0;
  std::uint8_t has_workload = 0;
  std::uint8_t has_process = 0;
  std::uint64_t region_generation = 0;
  std::uint64_t domain_generation = 0;
  std::uint64_t topology = 0;

  Status status = reader.u64(&event_id);
  if (!status.ok()) return Result<Observation>(status.error());
  status = require_enum(reader, &type, kEventTypeCount, "event type");
  if (!status.ok()) return Result<Observation>(status.error());
  status = read_name_id(reader, &observation.source_publisher);
  if (!status.ok()) return Result<Observation>(status.error());
  status = reader.u64(&boot);
  if (!status.ok()) return Result<Observation>(status.error());
  status = reader.u64(&coordinator);
  if (!status.ok()) return Result<Observation>(status.error());
  status = reader.u64(&sampling);
  if (!status.ok()) return Result<Observation>(status.error());
  status = reader.u64(&evidence);
  if (!status.ok()) return Result<Observation>(status.error());
  status = reader.u64(&sequence);
  if (!status.ok()) return Result<Observation>(status.error());
  status = reader.i64(&timestamp);
  if (!status.ok()) return Result<Observation>(status.error());
  status = read_optional_resource(reader, &observation.source);
  if (!status.ok()) return Result<Observation>(status.error());
  status = read_optional_resource(reader, &observation.target);
  if (!status.ok()) return Result<Observation>(status.error());
  status = reader.u8(&has_region);
  if (!status.ok()) return Result<Observation>(status.error());
  if (has_region != 0) {
    MemoryRegionId region;
    status = read_name_id(reader, &region);
    if (!status.ok()) return Result<Observation>(status.error());
    observation.region = region;
  }
  status = reader.u64(&region_generation);
  if (!status.ok()) return Result<Observation>(status.error());
  status = reader.u8(&has_domain);
  if (!status.ok()) return Result<Observation>(status.error());
  if (has_domain != 0) {
    CoherenceDomainId domain;
    status = read_name_id(reader, &domain);
    if (!status.ok()) return Result<Observation>(status.error());
    observation.coherence_domain = domain;
  }
  status = reader.u64(&domain_generation);
  if (!status.ok()) return Result<Observation>(status.error());
  status = reader.u64(&topology);
  if (!status.ok()) return Result<Observation>(status.error());
  status = reader.u8(&has_workload);
  if (!status.ok()) return Result<Observation>(status.error());
  if (has_workload != 0) {
    WorkloadId workload;
    status = read_name_id(reader, &workload);
    if (!status.ok()) return Result<Observation>(status.error());
    observation.workload = workload;
  }
  status = reader.u8(&has_process);
  if (!status.ok()) return Result<Observation>(status.error());
  if (has_process != 0) {
    ProcessId process;
    status = read_name_id(reader, &process);
    if (!status.ok()) return Result<Observation>(status.error());
    observation.process = process;
  }
  status = require_enum(reader, &direction, 6, "access direction");
  if (!status.ok()) return Result<Observation>(status.error());
  status = require_enum(reader, &state_before, 10, "coherence state");
  if (!status.ok()) return Result<Observation>(status.error());
  status = require_enum(reader, &state_after, 10, "coherence state");
  if (!status.ok()) return Result<Observation>(status.error());
  status = reader.u8(&has_counter);
  if (!status.ok()) return Result<Observation>(status.error());
  status = reader.u64(&counter_delta);
  if (!status.ok()) return Result<Observation>(status.error());
  status = reader.u64(&counter_generation);
  if (!status.ok()) return Result<Observation>(status.error());
  status = reader.u64(&observation.bytes);
  if (!status.ok()) return Result<Observation>(status.error());
  status = reader.u64(&observation.lines);
  if (!status.ok()) return Result<Observation>(status.error());
  status = reader.u64(&observation.pages);
  if (!status.ok()) return Result<Observation>(status.error());
  status = reader.u64(&observation.region_count);
  if (!status.ok()) return Result<Observation>(status.error());
  status = reader.i64(&measured);
  if (!status.ok()) return Result<Observation>(status.error());
  status = require_enum(reader, &locality, kLocalityCount, "locality");
  if (!status.ok()) return Result<Observation>(status.error());
  status = reader.u8(&locality_declared);
  if (!status.ok()) return Result<Observation>(status.error());
  status = require_enum(reader, &provenance, 10, "provenance");
  if (!status.ok()) return Result<Observation>(status.error());
  status = require_enum(reader, &precision, 7, "precision");
  if (!status.ok()) return Result<Observation>(status.error());
  status = require_enum(reader, &granularity, 7, "granularity");
  if (!status.ok()) return Result<Observation>(status.error());
  status = read_metadata(reader, &observation.metadata);
  if (!status.ok()) return Result<Observation>(status.error());

  observation.event_id = CoherenceEventId{event_id};
  observation.type = static_cast<EventType>(type);
  observation.publisher_boot = PublisherBootId{boot};
  observation.coordinator_epoch = CoordinatorEpoch{coordinator};
  observation.sampling_epoch = SamplingEpoch{sampling};
  observation.evidence_generation = EvidenceGeneration{evidence};
  observation.sequence = EventSequence{sequence};
  observation.timestamp_ns = timestamp;
  if (has_region != 0) {
    observation.region_generation = MemoryRegionGeneration{region_generation};
  }
  if (has_domain != 0) {
    observation.coherence_domain_generation = CoherenceDomainGeneration{domain_generation};
  }
  observation.topology_generation = TopologyGeneration{topology};
  observation.direction = static_cast<AccessDirection>(direction);
  observation.state_before = static_cast<CoherenceState>(state_before);
  observation.state_after = static_cast<CoherenceState>(state_after);
  if (has_counter != 0) {
    observation.counter_delta = counter_delta;
  }
  observation.counter_generation = CounterGeneration{counter_generation};
  observation.measured_duration_ns = measured;
  observation.locality = static_cast<Locality>(locality);
  observation.locality_declared = locality_declared != 0;
  observation.provenance = static_cast<Provenance>(provenance);
  observation.precision = static_cast<Precision>(precision);
  observation.granularity = static_cast<EvidenceGranularity>(granularity);
  return Result<Observation>(std::move(observation));
}

Status write_counter_publication(PayloadWriter& writer,
                                 const CounterPublication& publication) {
  Status status = write_name_id(writer, publication.publisher);
  if (!status.ok()) return status;
  writer.u64(publication.publisher_boot.value());
  writer.u64(publication.coordinator_epoch.value());
  writer.u64(publication.evidence_generation.value());
  status = write_name_id(writer, publication.counter);
  if (!status.ok()) return status;
  writer.u8(static_cast<std::uint8_t>(publication.mapped_type));
  writer.u8(static_cast<std::uint8_t>(publication.kind));
  writer.u8(static_cast<std::uint8_t>(publication.scope));
  writer.u32(publication.width_bits);
  writer.u64(publication.generation.value());
  writer.u64(publication.sampling_epoch.value());
  writer.u64(publication.raw_value);
  writer.u64(publication.bytes_per_unit);
  writer.u64(publication.sequence.value());
  writer.i64(publication.timestamp_ns);
  status = write_optional_resource(writer, publication.source);
  if (!status.ok()) return status;
  status = write_optional_resource(writer, publication.target);
  if (!status.ok()) return status;
  writer.u8(publication.region.has_value());
  if (publication.region.has_value()) {
    status = write_name_id(writer, *publication.region);
    if (!status.ok()) return status;
  }
  writer.u64(publication.region_generation.value_or(MemoryRegionGeneration{}).value());
  writer.u8(publication.workload.has_value());
  if (publication.workload.has_value()) {
    status = write_name_id(writer, *publication.workload);
    if (!status.ok()) return status;
  }
  writer.u64(publication.topology_generation.value());
  writer.u8(static_cast<std::uint8_t>(publication.provenance));
  writer.u8(static_cast<std::uint8_t>(publication.granularity));
  write_metadata(writer, publication.metadata);
  return Status();
}

Result<CounterPublication> read_counter_publication(PayloadReader& reader) {
  CounterPublication publication;
  std::uint8_t mapped_type = 0;
  std::uint8_t kind = 0;
  std::uint8_t scope = 0;
  std::uint8_t provenance = 0;
  std::uint8_t granularity = 0;
  std::uint8_t has_region = 0;
  std::uint8_t has_workload = 0;
  std::uint64_t boot = 0;
  std::uint64_t coordinator = 0;
  std::uint64_t evidence = 0;
  std::uint64_t generation = 0;
  std::uint64_t sampling = 0;
  std::uint64_t sequence = 0;
  std::uint64_t region_generation = 0;
  std::uint64_t topology = 0;
  std::int64_t timestamp = 0;

  Status status = read_name_id(reader, &publication.publisher);
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = reader.u64(&boot);
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = reader.u64(&coordinator);
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = reader.u64(&evidence);
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = read_name_id(reader, &publication.counter);
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = require_enum(reader, &mapped_type, kEventTypeCount, "counter event type");
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = require_enum(reader, &kind, 5, "counter kind");
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = require_enum(reader, &scope, 7, "counter scope");
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = reader.u32(&publication.width_bits);
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = reader.u64(&generation);
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = reader.u64(&sampling);
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = reader.u64(&publication.raw_value);
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = reader.u64(&publication.bytes_per_unit);
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = reader.u64(&sequence);
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = reader.i64(&timestamp);
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = read_optional_resource(reader, &publication.source);
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = read_optional_resource(reader, &publication.target);
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = reader.u8(&has_region);
  if (!status.ok()) return Result<CounterPublication>(status.error());
  if (has_region != 0) {
    MemoryRegionId region;
    status = read_name_id(reader, &region);
    if (!status.ok()) return Result<CounterPublication>(status.error());
    publication.region = region;
  }
  status = reader.u64(&region_generation);
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = reader.u8(&has_workload);
  if (!status.ok()) return Result<CounterPublication>(status.error());
  if (has_workload != 0) {
    WorkloadId workload;
    status = read_name_id(reader, &workload);
    if (!status.ok()) return Result<CounterPublication>(status.error());
    publication.workload = workload;
  }
  status = reader.u64(&topology);
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = require_enum(reader, &provenance, 10, "counter provenance");
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = require_enum(reader, &granularity, 7, "counter granularity");
  if (!status.ok()) return Result<CounterPublication>(status.error());
  status = read_metadata(reader, &publication.metadata);
  if (!status.ok()) return Result<CounterPublication>(status.error());

  publication.publisher_boot = PublisherBootId{boot};
  publication.coordinator_epoch = CoordinatorEpoch{coordinator};
  publication.evidence_generation = EvidenceGeneration{evidence};
  publication.mapped_type = static_cast<EventType>(mapped_type);
  publication.kind = static_cast<CounterKind>(kind);
  publication.scope = static_cast<CounterScope>(scope);
  publication.generation = CounterGeneration{generation};
  publication.sampling_epoch = SamplingEpoch{sampling};
  publication.sequence = EventSequence{sequence};
  publication.timestamp_ns = timestamp;
  if (has_region != 0) {
    publication.region_generation = MemoryRegionGeneration{region_generation};
  }
  publication.topology_generation = TopologyGeneration{topology};
  publication.provenance = static_cast<Provenance>(provenance);
  publication.granularity = static_cast<EvidenceGranularity>(granularity);
  return Result<CounterPublication>(std::move(publication));
}

Status write_publisher_registration(PayloadWriter& writer,
                                    const PublisherRegistration& registration) {
  Status status = write_name_id(writer, registration.id);
  if (!status.ok()) return status;
  writer.u64(registration.boot.value());
  status = write_name_id(writer, registration.observer);
  if (!status.ok()) return status;
  status = write_name_id(writer, registration.node);
  if (!status.ok()) return status;
  status = writer.text(registration.display_name, kMaxTextAnnotation);
  if (!status.ok()) return status;
  writer.u64(registration.coordinator_epoch.value());
  writer.u64(registration.evidence_generation.value());
  writer.u64(registration.sampling_epoch.value());
  writer.u8(static_cast<std::uint8_t>(registration.provenance));
  return Status();
}

Result<PublisherRegistration> read_publisher_registration(PayloadReader& reader) {
  PublisherRegistration registration;
  std::uint64_t boot = 0;
  std::uint64_t coordinator = 0;
  std::uint64_t evidence = 0;
  std::uint64_t sampling = 0;
  std::uint8_t provenance = 0;
  Status status = read_name_id(reader, &registration.id);
  if (!status.ok()) return Result<PublisherRegistration>(status.error());
  status = reader.u64(&boot);
  if (!status.ok()) return Result<PublisherRegistration>(status.error());
  status = read_name_id(reader, &registration.observer);
  if (!status.ok()) return Result<PublisherRegistration>(status.error());
  status = read_name_id(reader, &registration.node);
  if (!status.ok()) return Result<PublisherRegistration>(status.error());
  status = reader.text(&registration.display_name, kMaxTextAnnotation);
  if (!status.ok()) return Result<PublisherRegistration>(status.error());
  status = reader.u64(&coordinator);
  if (!status.ok()) return Result<PublisherRegistration>(status.error());
  status = reader.u64(&evidence);
  if (!status.ok()) return Result<PublisherRegistration>(status.error());
  status = reader.u64(&sampling);
  if (!status.ok()) return Result<PublisherRegistration>(status.error());
  status = require_enum(reader, &provenance, 10, "publisher provenance");
  if (!status.ok()) return Result<PublisherRegistration>(status.error());
  registration.boot = PublisherBootId{boot};
  registration.coordinator_epoch = CoordinatorEpoch{coordinator};
  registration.evidence_generation = EvidenceGeneration{evidence};
  registration.sampling_epoch = SamplingEpoch{sampling};
  registration.provenance = static_cast<Provenance>(provenance);
  return Result<PublisherRegistration>(std::move(registration));
}

Status write_node(PayloadWriter& writer, const NodeRecord& record) {
  Status status = write_name_id(writer, record.id);
  if (!status.ok()) return status;
  status = writer.text(record.display_name, kMaxTextAnnotation);
  if (!status.ok()) return status;
  writer.u64(static_cast<std::uint64_t>(record.registered_at_ns));
  return Status();
}

Result<NodeRecord> read_node(PayloadReader& reader) {
  NodeRecord record;
  Status status = read_name_id(reader, &record.id);
  if (!status.ok()) return Result<NodeRecord>(status.error());
  status = reader.text(&record.display_name, kMaxTextAnnotation);
  if (!status.ok()) return Result<NodeRecord>(status.error());
  std::uint64_t registered = 0;
  status = reader.u64(&registered);
  if (!status.ok()) return Result<NodeRecord>(status.error());
  record.registered_at_ns = static_cast<Nanos>(registered);
  return Result<NodeRecord>(std::move(record));
}

Status write_processor(PayloadWriter& writer, const ProcessorRecord& record) {
  Status status = write_name_id(writer, record.id);
  if (!status.ok()) return status;
  status = write_name_id(writer, record.node);
  if (!status.ok()) return status;
  status = write_name_id(writer, record.local_memory_domain);
  if (!status.ok()) return status;
  writer.u64(record.generation.value());
  writer.u32(record.logical_processor_count);
  writer.u32(record.physical_core_count);
  writer.u32(record.numa_node_index);
  writer.u8(record.numa_node_index_known);
  writer.u8(record.retired);
  writer.u64(static_cast<std::uint64_t>(record.registered_at_ns));
  return Status();
}

Result<ProcessorRecord> read_processor(PayloadReader& reader) {
  ProcessorRecord record;
  std::uint64_t generation = 0;
  std::uint8_t numa_known = 0;
  std::uint8_t retired = 0;
  std::uint64_t registered = 0;
  Status status = read_name_id(reader, &record.id);
  if (!status.ok()) return Result<ProcessorRecord>(status.error());
  status = read_name_id(reader, &record.node);
  if (!status.ok()) return Result<ProcessorRecord>(status.error());
  status = read_name_id(reader, &record.local_memory_domain);
  if (!status.ok()) return Result<ProcessorRecord>(status.error());
  status = reader.u64(&generation);
  if (!status.ok()) return Result<ProcessorRecord>(status.error());
  status = reader.u32(&record.logical_processor_count);
  if (!status.ok()) return Result<ProcessorRecord>(status.error());
  status = reader.u32(&record.physical_core_count);
  if (!status.ok()) return Result<ProcessorRecord>(status.error());
  status = reader.u32(&record.numa_node_index);
  if (!status.ok()) return Result<ProcessorRecord>(status.error());
  status = reader.u8(&numa_known);
  if (!status.ok()) return Result<ProcessorRecord>(status.error());
  status = reader.u8(&retired);
  if (!status.ok()) return Result<ProcessorRecord>(status.error());
  status = reader.u64(&registered);
  if (!status.ok()) return Result<ProcessorRecord>(status.error());
  record.generation = DeviceGeneration{generation};
  record.numa_node_index_known = numa_known != 0;
  record.retired = retired != 0;
  record.registered_at_ns = static_cast<Nanos>(registered);
  return Result<ProcessorRecord>(std::move(record));
}

Status write_accelerator(PayloadWriter& writer, const AcceleratorRecord& record) {
  Status status = write_name_id(writer, record.id);
  if (!status.ok()) return status;
  status = write_name_id(writer, record.node);
  if (!status.ok()) return status;
  status = writer.text(record.vendor, kMaxTextShort);
  if (!status.ok()) return status;
  status = writer.text(record.vendor_uuid, kMaxTextAnnotation);
  if (!status.ok()) return status;
  status = write_name_id(writer, record.local_memory_domain);
  if (!status.ok()) return status;
  writer.u64(record.generation.value());
  writer.u8(record.peer_coherent_capable);
  writer.u8(record.peer_coherent_capability_known);
  writer.u8(record.retired);
  writer.u64(static_cast<std::uint64_t>(record.registered_at_ns));
  return Status();
}

Result<AcceleratorRecord> read_accelerator(PayloadReader& reader) {
  AcceleratorRecord record;
  std::uint64_t generation = 0;
  std::uint8_t peer_capable = 0;
  std::uint8_t peer_known = 0;
  std::uint8_t retired = 0;
  std::uint64_t registered = 0;
  Status status = read_name_id(reader, &record.id);
  if (!status.ok()) return Result<AcceleratorRecord>(status.error());
  status = read_name_id(reader, &record.node);
  if (!status.ok()) return Result<AcceleratorRecord>(status.error());
  status = reader.text(&record.vendor, kMaxTextShort);
  if (!status.ok()) return Result<AcceleratorRecord>(status.error());
  status = reader.text(&record.vendor_uuid, kMaxTextAnnotation);
  if (!status.ok()) return Result<AcceleratorRecord>(status.error());
  status = read_name_id(reader, &record.local_memory_domain);
  if (!status.ok()) return Result<AcceleratorRecord>(status.error());
  status = reader.u64(&generation);
  if (!status.ok()) return Result<AcceleratorRecord>(status.error());
  status = reader.u8(&peer_capable);
  if (!status.ok()) return Result<AcceleratorRecord>(status.error());
  status = reader.u8(&peer_known);
  if (!status.ok()) return Result<AcceleratorRecord>(status.error());
  status = reader.u8(&retired);
  if (!status.ok()) return Result<AcceleratorRecord>(status.error());
  status = reader.u64(&registered);
  if (!status.ok()) return Result<AcceleratorRecord>(status.error());
  record.generation = DeviceGeneration{generation};
  record.peer_coherent_capable = peer_capable != 0;
  record.peer_coherent_capability_known = peer_known != 0;
  record.retired = retired != 0;
  record.registered_at_ns = static_cast<Nanos>(registered);
  return Result<AcceleratorRecord>(std::move(record));
}

Status write_memory_domain(PayloadWriter& writer, const MemoryDomainRecord& record) {
  Status status = write_name_id(writer, record.id);
  if (!status.ok()) return status;
  status = write_name_id(writer, record.node);
  if (!status.ok()) return status;
  status = write_name_id(writer, record.coherence_domain);
  if (!status.ok()) return status;
  writer.u64(record.generation.value());
  writer.u8(static_cast<std::uint8_t>(record.kind));
  writer.u8(record.coherent_with_host);
  writer.u8(record.coherent_with_host_known);
  writer.u8(record.capacity_known);
  writer.u8(record.retired);
  writer.u64(record.capacity_bytes);
  writer.u64(static_cast<std::uint64_t>(record.registered_at_ns));
  return Status();
}

Result<MemoryDomainRecord> read_memory_domain(PayloadReader& reader) {
  MemoryDomainRecord record;
  std::uint64_t generation = 0;
  std::uint8_t kind = 0;
  std::uint8_t coherent = 0;
  std::uint8_t coherent_known = 0;
  std::uint8_t capacity_known = 0;
  std::uint8_t retired = 0;
  std::uint64_t registered = 0;
  Status status = read_name_id(reader, &record.id);
  if (!status.ok()) return Result<MemoryDomainRecord>(status.error());
  status = read_name_id(reader, &record.node);
  if (!status.ok()) return Result<MemoryDomainRecord>(status.error());
  status = read_name_id(reader, &record.coherence_domain);
  if (!status.ok()) return Result<MemoryDomainRecord>(status.error());
  status = reader.u64(&generation);
  if (!status.ok()) return Result<MemoryDomainRecord>(status.error());
  status = require_enum(reader, &kind, 7, "memory domain kind");
  if (!status.ok()) return Result<MemoryDomainRecord>(status.error());
  status = reader.u8(&coherent);
  if (!status.ok()) return Result<MemoryDomainRecord>(status.error());
  status = reader.u8(&coherent_known);
  if (!status.ok()) return Result<MemoryDomainRecord>(status.error());
  status = reader.u8(&capacity_known);
  if (!status.ok()) return Result<MemoryDomainRecord>(status.error());
  status = reader.u8(&retired);
  if (!status.ok()) return Result<MemoryDomainRecord>(status.error());
  status = reader.u64(&record.capacity_bytes);
  if (!status.ok()) return Result<MemoryDomainRecord>(status.error());
  status = reader.u64(&registered);
  if (!status.ok()) return Result<MemoryDomainRecord>(status.error());
  record.generation = MemoryDomainGeneration{generation};
  record.kind = static_cast<MemoryDomainKind>(kind);
  record.coherent_with_host = coherent != 0;
  record.coherent_with_host_known = coherent_known != 0;
  record.capacity_known = capacity_known != 0;
  record.retired = retired != 0;
  record.registered_at_ns = static_cast<Nanos>(registered);
  return Result<MemoryDomainRecord>(std::move(record));
}

Status write_coherence_domain(PayloadWriter& writer, const CoherenceDomainRecord& record) {
  Status status = write_name_id(writer, record.id);
  if (!status.ok()) return status;
  status = writer.text(record.protocol_family, kMaxTextAnnotation);
  if (!status.ok()) return status;
  writer.u64(record.generation.value());
  writer.u8(record.retired);
  writer.u32(static_cast<std::uint32_t>(record.members.size()));
  for (const ResourceRef& member : record.members) {
    status = write_resource_ref(writer, member);
    if (!status.ok()) return status;
  }
  writer.u64(static_cast<std::uint64_t>(record.registered_at_ns));
  return Status();
}

Result<CoherenceDomainRecord> read_coherence_domain(PayloadReader& reader) {
  CoherenceDomainRecord record;
  std::uint64_t generation = 0;
  std::uint8_t retired = 0;
  std::uint32_t member_count = 0;
  std::uint64_t registered = 0;
  Status status = read_name_id(reader, &record.id);
  if (!status.ok()) return Result<CoherenceDomainRecord>(status.error());
  status = reader.text(&record.protocol_family, kMaxTextAnnotation);
  if (!status.ok()) return Result<CoherenceDomainRecord>(status.error());
  status = reader.u64(&generation);
  if (!status.ok()) return Result<CoherenceDomainRecord>(status.error());
  status = reader.u8(&retired);
  if (!status.ok()) return Result<CoherenceDomainRecord>(status.error());
  status = reader.u32(&member_count);
  if (!status.ok()) return Result<CoherenceDomainRecord>(status.error());
  if (member_count > Limits::kMaxTopologyLinkCount) {
    return fail_as<CoherenceDomainRecord>(ErrorCode::TooMany, "coherence domain member count");
  }
  for (std::uint32_t i = 0; i < member_count; ++i) {
    ResourceRef member;
    status = read_resource_ref(reader, &member);
    if (!status.ok()) return Result<CoherenceDomainRecord>(status.error());
    record.members.push_back(std::move(member));
  }
  status = reader.u64(&registered);
  if (!status.ok()) return Result<CoherenceDomainRecord>(status.error());
  record.generation = CoherenceDomainGeneration{generation};
  record.retired = retired != 0;
  record.registered_at_ns = static_cast<Nanos>(registered);
  return Result<CoherenceDomainRecord>(std::move(record));
}

Status write_region(PayloadWriter& writer, const RegionRecord& record) {
  Status status = write_name_id(writer, record.id);
  if (!status.ok()) return status;
  status = writer.text(record.owner, kMaxTextAnnotation);
  if (!status.ok()) return status;
  status = writer.text(record.annotation, kMaxTextAnnotation);
  if (!status.ok()) return status;
  status = write_name_id(writer, record.memory_domain);
  if (!status.ok()) return status;
  status = write_name_id(writer, record.coherence_domain);
  if (!status.ok()) return status;
  status = write_name_id(writer, record.workload);
  if (!status.ok()) return status;
  writer.u64(record.generation.value());
  writer.u64(record.memory_domain_generation.value());
  writer.u64(record.opaque_handle);
  writer.u64(record.size_bytes);
  writer.u64(record.page_size_bytes);
  writer.u64(record.allocation_generation);
  writer.u64(record.mapping_generation);
  writer.u8(static_cast<std::uint8_t>(record.sharing_scope));
  writer.u8(record.size_known);
  writer.u8(record.page_size_known);
  writer.u8(record.retired);
  writer.u64(static_cast<std::uint64_t>(record.registered_at_ns));
  writer.u64(static_cast<std::uint64_t>(record.retired_at_ns));
  return Status();
}

Result<RegionRecord> read_region(PayloadReader& reader) {
  RegionRecord record;
  std::uint64_t generation = 0;
  std::uint64_t domain_generation = 0;
  std::uint64_t registered = 0;
  std::uint64_t retired_at = 0;
  std::uint8_t scope = 0;
  std::uint8_t size_known = 0;
  std::uint8_t page_known = 0;
  std::uint8_t retired = 0;
  Status status = read_name_id(reader, &record.id);
  if (!status.ok()) return Result<RegionRecord>(status.error());
  status = reader.text(&record.owner, kMaxTextAnnotation);
  if (!status.ok()) return Result<RegionRecord>(status.error());
  status = reader.text(&record.annotation, kMaxTextAnnotation);
  if (!status.ok()) return Result<RegionRecord>(status.error());
  status = read_name_id(reader, &record.memory_domain);
  if (!status.ok()) return Result<RegionRecord>(status.error());
  status = read_name_id(reader, &record.coherence_domain);
  if (!status.ok()) return Result<RegionRecord>(status.error());
  status = read_name_id(reader, &record.workload);
  if (!status.ok()) return Result<RegionRecord>(status.error());
  status = reader.u64(&generation);
  if (!status.ok()) return Result<RegionRecord>(status.error());
  status = reader.u64(&domain_generation);
  if (!status.ok()) return Result<RegionRecord>(status.error());
  status = reader.u64(&record.opaque_handle);
  if (!status.ok()) return Result<RegionRecord>(status.error());
  status = reader.u64(&record.size_bytes);
  if (!status.ok()) return Result<RegionRecord>(status.error());
  status = reader.u64(&record.page_size_bytes);
  if (!status.ok()) return Result<RegionRecord>(status.error());
  status = reader.u64(&record.allocation_generation);
  if (!status.ok()) return Result<RegionRecord>(status.error());
  status = reader.u64(&record.mapping_generation);
  if (!status.ok()) return Result<RegionRecord>(status.error());
  status = require_enum(reader, &scope, 6, "sharing scope");
  if (!status.ok()) return Result<RegionRecord>(status.error());
  status = reader.u8(&size_known);
  if (!status.ok()) return Result<RegionRecord>(status.error());
  status = reader.u8(&page_known);
  if (!status.ok()) return Result<RegionRecord>(status.error());
  status = reader.u8(&retired);
  if (!status.ok()) return Result<RegionRecord>(status.error());
  status = reader.u64(&registered);
  if (!status.ok()) return Result<RegionRecord>(status.error());
  status = reader.u64(&retired_at);
  if (!status.ok()) return Result<RegionRecord>(status.error());
  record.generation = MemoryRegionGeneration{generation};
  record.memory_domain_generation = MemoryDomainGeneration{domain_generation};
  record.sharing_scope = static_cast<SharingScope>(scope);
  record.size_known = size_known != 0;
  record.page_size_known = page_known != 0;
  record.retired = retired != 0;
  record.registered_at_ns = static_cast<Nanos>(registered);
  record.retired_at_ns = static_cast<Nanos>(retired_at);
  return Result<RegionRecord>(std::move(record));
}

Status write_topology_link(PayloadWriter& writer, const TopologyLink& link) {
  Status status = write_resource_ref(writer, link.from);
  if (!status.ok()) return status;
  status = write_resource_ref(writer, link.to);
  if (!status.ok()) return status;
  writer.u8(static_cast<std::uint8_t>(link.locality));
  writer.u8(static_cast<std::uint8_t>(link.provenance));
  writer.i64(link.latency_ns);
  writer.u64(link.bandwidth_bytes_per_second);
  return Status();
}

Result<TopologyLink> read_topology_link(PayloadReader& reader) {
  TopologyLink link;
  std::uint8_t locality = 0;
  std::uint8_t provenance = 0;
  Status status = read_resource_ref(reader, &link.from);
  if (!status.ok()) return Result<TopologyLink>(status.error());
  status = read_resource_ref(reader, &link.to);
  if (!status.ok()) return Result<TopologyLink>(status.error());
  status = require_enum(reader, &locality, kLocalityCount, "locality");
  if (!status.ok()) return Result<TopologyLink>(status.error());
  status = require_enum(reader, &provenance, 10, "provenance");
  if (!status.ok()) return Result<TopologyLink>(status.error());
  status = reader.i64(&link.latency_ns);
  if (!status.ok()) return Result<TopologyLink>(status.error());
  status = reader.u64(&link.bandwidth_bytes_per_second);
  if (!status.ok()) return Result<TopologyLink>(status.error());
  link.locality = static_cast<Locality>(locality);
  link.provenance = static_cast<Provenance>(provenance);
  return Result<TopologyLink>(std::move(link));
}

Status write_capability(PayloadWriter& writer, const Capability& capability) {
  Status status = writer.text(capability.key, kMaxTextShort);
  if (!status.ok()) return status;
  writer.u8(static_cast<std::uint8_t>(capability.status));
  status = writer.text(capability.detail, 512);
  if (!status.ok()) return status;
  return writer.text(capability.mechanism, 128);
}

Result<Capability> read_capability(PayloadReader& reader) {
  Capability capability;
  std::uint8_t capability_status = 0;
  Status status = reader.text(&capability.key, kMaxTextShort);
  if (!status.ok()) return Result<Capability>(status.error());
  status = require_enum(reader, &capability_status, 3, "capability status");
  if (!status.ok()) return Result<Capability>(status.error());
  status = reader.text(&capability.detail, 512);
  if (!status.ok()) return Result<Capability>(status.error());
  status = reader.text(&capability.mechanism, 128);
  if (!status.ok()) return Result<Capability>(status.error());
  capability.status = static_cast<CapabilityStatus>(capability_status);
  return Result<Capability>(std::move(capability));
}

Status write_finding(PayloadWriter& writer, const Finding& finding) {
  writer.u64(finding.id.value());
  writer.u8(static_cast<std::uint8_t>(finding.kind));
  Status status = writer.text(finding.subject_kind, 32);
  if (!status.ok()) return status;
  status = writer.text(finding.subject, kMaxTextAnnotation);
  if (!status.ok()) return status;
  status = write_string_list(writer, finding.reasons, 32, kMaxTextShort);
  if (!status.ok()) return status;
  status = write_string_list(writer, finding.missing_evidence, 32, 256);
  if (!status.ok()) return status;
  status = write_string_list(writer, finding.ambiguity, 32, 256);
  if (!status.ok()) return status;
  status = write_string_list(writer, finding.participants, 32, kMaxTextShort);
  if (!status.ok()) return status;
  writer.u32(static_cast<std::uint32_t>(finding.metrics.size()));
  for (const FindingMetric& metric : finding.metrics) {
    status = writer.text(metric.name, kMaxTextShort);
    if (!status.ok()) return status;
    writer.f64(metric.value);
    status = writer.text(metric.unit, 48);
    if (!status.ok()) return status;
    writer.f64(metric.threshold);
    writer.u8(metric.has_threshold);
    writer.u8(metric.exceeded);
  }
  write_evidence(writer, finding.evidence);
  writer.u8(static_cast<std::uint8_t>(finding.precision));
  writer.u8(static_cast<std::uint8_t>(finding.provenance));
  writer.u8(static_cast<std::uint8_t>(finding.reality));
  write_bindings(writer, finding.bindings);
  write_cost(writer, finding.cost);
  writer.u64(finding.alternations);
  status = writer.text(finding.direction_sequence, 192);
  if (!status.ok()) return status;
  writer.u8(static_cast<std::uint8_t>(finding.contention));
  writer.u8(static_cast<std::uint8_t>(finding.granularity));
  writer.u8(static_cast<std::uint8_t>(finding.locality));
  writer.u8(finding.locality_established);
  writer.u8(finding.historical);
  return Status();
}

Result<Finding> read_finding(PayloadReader& reader) {
  Finding finding;
  std::uint64_t id = 0;
  std::uint8_t kind = 0;
  std::uint8_t precision = 0;
  std::uint8_t provenance = 0;
  std::uint8_t reality = 0;
  std::uint8_t contention = 0;
  std::uint8_t granularity = 0;
  std::uint8_t locality = 0;
  std::uint8_t locality_established = 0;
  std::uint8_t historical = 0;
  Status status = reader.u64(&id);
  if (!status.ok()) return Result<Finding>(status.error());
  status = require_enum(reader, &kind, 12, "finding kind");
  if (!status.ok()) return Result<Finding>(status.error());
  status = reader.text(&finding.subject_kind, 32);
  if (!status.ok()) return Result<Finding>(status.error());
  status = reader.text(&finding.subject, kMaxTextAnnotation);
  if (!status.ok()) return Result<Finding>(status.error());
  status = read_string_list(reader, &finding.reasons, 32, kMaxTextShort);
  if (!status.ok()) return Result<Finding>(status.error());
  status = read_string_list(reader, &finding.missing_evidence, 32, 256);
  if (!status.ok()) return Result<Finding>(status.error());
  status = read_string_list(reader, &finding.ambiguity, 32, 256);
  if (!status.ok()) return Result<Finding>(status.error());
  status = read_string_list(reader, &finding.participants, 32, kMaxTextShort);
  if (!status.ok()) return Result<Finding>(status.error());
  std::uint32_t metric_count = 0;
  status = reader.u32(&metric_count);
  if (!status.ok()) return Result<Finding>(status.error());
  if (metric_count > 128) {
    return fail_as<Finding>(ErrorCode::TooMany, "finding metric count exceeds the bound");
  }
  for (std::uint32_t i = 0; i < metric_count; ++i) {
    FindingMetric metric;
    std::uint8_t has_threshold = 0;
    std::uint8_t exceeded = 0;
    status = reader.text(&metric.name, kMaxTextShort);
    if (!status.ok()) return Result<Finding>(status.error());
    status = reader.f64(&metric.value);
    if (!status.ok()) return Result<Finding>(status.error());
    status = reader.text(&metric.unit, 48);
    if (!status.ok()) return Result<Finding>(status.error());
    status = reader.f64(&metric.threshold);
    if (!status.ok()) return Result<Finding>(status.error());
    status = reader.u8(&has_threshold);
    if (!status.ok()) return Result<Finding>(status.error());
    status = reader.u8(&exceeded);
    if (!status.ok()) return Result<Finding>(status.error());
    metric.has_threshold = has_threshold != 0;
    metric.exceeded = exceeded != 0;
    finding.metrics.push_back(std::move(metric));
  }
  status = read_evidence(reader, &finding.evidence);
  if (!status.ok()) return Result<Finding>(status.error());
  status = require_enum(reader, &precision, 7, "finding precision");
  if (!status.ok()) return Result<Finding>(status.error());
  status = require_enum(reader, &provenance, 10, "finding provenance");
  if (!status.ok()) return Result<Finding>(status.error());
  status = require_enum(reader, &reality, 3, "finding reality");
  if (!status.ok()) return Result<Finding>(status.error());
  status = read_bindings(reader, &finding.bindings);
  if (!status.ok()) return Result<Finding>(status.error());
  status = read_cost(reader, &finding.cost);
  if (!status.ok()) return Result<Finding>(status.error());
  status = reader.u64(&finding.alternations);
  if (!status.ok()) return Result<Finding>(status.error());
  status = reader.text(&finding.direction_sequence, 192);
  if (!status.ok()) return Result<Finding>(status.error());
  status = require_enum(reader, &contention, 5, "contention class");
  if (!status.ok()) return Result<Finding>(status.error());
  status = require_enum(reader, &granularity, 7, "finding granularity");
  if (!status.ok()) return Result<Finding>(status.error());
  status = require_enum(reader, &locality, kLocalityCount, "finding locality");
  if (!status.ok()) return Result<Finding>(status.error());
  status = reader.u8(&locality_established);
  if (!status.ok()) return Result<Finding>(status.error());
  status = reader.u8(&historical);
  if (!status.ok()) return Result<Finding>(status.error());
  finding.id = FindingId{id};
  finding.kind = static_cast<FindingKind>(kind);
  finding.precision = static_cast<Precision>(precision);
  finding.provenance = static_cast<Provenance>(provenance);
  finding.reality = static_cast<Reality>(reality);
  finding.contention = static_cast<ContentionClass>(contention);
  finding.granularity = static_cast<EvidenceGranularity>(granularity);
  finding.locality = static_cast<Locality>(locality);
  finding.locality_established = locality_established != 0;
  finding.historical = historical != 0;
  return Result<Finding>(std::move(finding));
}

Status write_attribution(PayloadWriter& writer, const AttributionResult& attribution) {
  writer.u64(attribution.id.value());
  writer.u8(static_cast<std::uint8_t>(attribution.outcome));
  writer.u8(static_cast<std::uint8_t>(attribution.event_type));
  Status status = write_name_id(writer, attribution.region);
  if (!status.ok()) return status;
  writer.u64(attribution.region_generation.value());
  status = write_string_list(writer, attribution.reasons, 32, kMaxTextShort);
  if (!status.ok()) return status;
  status = write_string_list(writer, attribution.candidates, 32, kMaxTextAnnotation);
  if (!status.ok()) return status;
  writer.u32(static_cast<std::uint32_t>(attribution.targets.size()));
  for (const AttributionTarget& target : attribution.targets) {
    writer.u8(static_cast<std::uint8_t>(target.kind));
    status = writer.text(target.value, kMaxTextAnnotation);
    if (!status.ok()) return status;
    writer.u64(target.generation);
  }
  write_evidence(writer, attribution.evidence);
  writer.u8(static_cast<std::uint8_t>(attribution.precision));
  writer.u8(static_cast<std::uint8_t>(attribution.provenance));
  writer.u8(static_cast<std::uint8_t>(attribution.reality));
  write_bindings(writer, attribution.bindings);
  writer.u64(attribution.candidate_count);
  write_cost(writer, attribution.cost);
  writer.u8(static_cast<std::uint8_t>(attribution.locality));
  writer.u8(attribution.locality_established);
  return Status();
}

Result<AttributionResult> read_attribution(PayloadReader& reader) {
  AttributionResult attribution;
  std::uint64_t id = 0;
  std::uint8_t outcome = 0;
  std::uint8_t event_type = 0;
  std::uint8_t precision = 0;
  std::uint8_t provenance = 0;
  std::uint8_t reality = 0;
  std::uint8_t locality = 0;
  std::uint8_t locality_established = 0;
  std::uint64_t region_generation = 0;
  Status status = reader.u64(&id);
  if (!status.ok()) return Result<AttributionResult>(status.error());
  status = require_enum(reader, &outcome, 7, "attribution outcome");
  if (!status.ok()) return Result<AttributionResult>(status.error());
  status = require_enum(reader, &event_type, kEventTypeCount, "attribution event type");
  if (!status.ok()) return Result<AttributionResult>(status.error());
  status = read_name_id(reader, &attribution.region);
  if (!status.ok()) return Result<AttributionResult>(status.error());
  status = reader.u64(&region_generation);
  if (!status.ok()) return Result<AttributionResult>(status.error());
  status = read_string_list(reader, &attribution.reasons, 32, kMaxTextShort);
  if (!status.ok()) return Result<AttributionResult>(status.error());
  status = read_string_list(reader, &attribution.candidates, 32, kMaxTextAnnotation);
  if (!status.ok()) return Result<AttributionResult>(status.error());
  std::uint32_t target_count = 0;
  status = reader.u32(&target_count);
  if (!status.ok()) return Result<AttributionResult>(status.error());
  if (target_count > Limits::kMaxAttributionTargets) {
    return fail_as<AttributionResult>(ErrorCode::TooMany, "attribution target count");
  }
  for (std::uint32_t i = 0; i < target_count; ++i) {
    AttributionTarget target;
    std::uint8_t kind = 0;
    status = require_enum(reader, &kind, 9, "attribution target kind");
    if (!status.ok()) return Result<AttributionResult>(status.error());
    status = reader.text(&target.value, kMaxTextAnnotation);
    if (!status.ok()) return Result<AttributionResult>(status.error());
    status = reader.u64(&target.generation);
    if (!status.ok()) return Result<AttributionResult>(status.error());
    target.kind = static_cast<AttributionTargetKind>(kind);
    attribution.targets.push_back(std::move(target));
  }
  status = read_evidence(reader, &attribution.evidence);
  if (!status.ok()) return Result<AttributionResult>(status.error());
  status = require_enum(reader, &precision, 7, "attribution precision");
  if (!status.ok()) return Result<AttributionResult>(status.error());
  status = require_enum(reader, &provenance, 10, "attribution provenance");
  if (!status.ok()) return Result<AttributionResult>(status.error());
  status = require_enum(reader, &reality, 3, "attribution reality");
  if (!status.ok()) return Result<AttributionResult>(status.error());
  status = read_bindings(reader, &attribution.bindings);
  if (!status.ok()) return Result<AttributionResult>(status.error());
  status = reader.u64(&attribution.candidate_count);
  if (!status.ok()) return Result<AttributionResult>(status.error());
  status = read_cost(reader, &attribution.cost);
  if (!status.ok()) return Result<AttributionResult>(status.error());
  status = require_enum(reader, &locality, kLocalityCount, "attribution locality");
  if (!status.ok()) return Result<AttributionResult>(status.error());
  status = reader.u8(&locality_established);
  if (!status.ok()) return Result<AttributionResult>(status.error());
  attribution.id = AttributionId{id};
  attribution.outcome = static_cast<AttributionOutcome>(outcome);
  attribution.event_type = static_cast<EventType>(event_type);
  attribution.region_generation = MemoryRegionGeneration{region_generation};
  attribution.precision = static_cast<Precision>(precision);
  attribution.provenance = static_cast<Provenance>(provenance);
  attribution.reality = static_cast<Reality>(reality);
  attribution.locality = static_cast<Locality>(locality);
  attribution.locality_established = locality_established != 0;
  return Result<AttributionResult>(std::move(attribution));
}

Status write_snapshot(PayloadWriter& writer, const Snapshot& snapshot) {
  writer.u64(snapshot.generation().value());
  writer.u64(snapshot.coordinator_epoch().value());
  writer.u64(snapshot.observation_epoch().value());
  writer.u64(snapshot.topology_generation().value());
  writer.i64(snapshot.captured_at_ns());
  writer.u8(static_cast<std::uint8_t>(snapshot.reality()));
  writer.u64(snapshot.historical_observation_count());
  writer.u64(snapshot.history_source_epoch().value());
  writer.u64(snapshot.historical_aggregates().size());
  writer.u64(snapshot.history().size());

  writer.u32(static_cast<std::uint32_t>(snapshot.publishers().size()));
  for (const PublisherView& publisher : snapshot.publishers()) {
    Status status = write_name_id(writer, publisher.id);
    if (!status.ok()) return status;
    writer.u64(publisher.boot.value());
    status = write_name_id(writer, publisher.observer);
    if (!status.ok()) return status;
    status = write_name_id(writer, publisher.node);
    if (!status.ok()) return status;
    status = writer.text(publisher.display_name, kMaxTextAnnotation);
    if (!status.ok()) return status;
    writer.u8(static_cast<std::uint8_t>(publisher.provenance));
    writer.u8(static_cast<std::uint8_t>(publisher.reality));
    writer.u64(publisher.registered_epoch.value());
    writer.u64(publisher.evidence_generation.value());
    writer.u64(publisher.sampling_epoch.value());
    writer.i64(publisher.registered_at_ns);
    writer.i64(publisher.last_seen_ns);
    writer.u8(publisher.current);
    writer.u8(publisher.fenced);
    writer.u8(static_cast<std::uint8_t>(publisher.fence_reason));
    status = writer.text(publisher.fence_detail, kMaxTextAnnotation);
    if (!status.ok()) return status;
    writer.u8(publisher.loaded_from_state);
    writer.u64(publisher.sequences.high_watermark.value());
    writer.u64(publisher.sequences.next_expected.value());
    writer.u64(publisher.sequences.accepted);
    writer.u64(publisher.sequences.duplicates);
    writer.u64(publisher.sequences.conflicting_duplicates);
    writer.u64(publisher.sequences.late_events);
    writer.u64(publisher.sequences.missing_events);
    writer.u64(publisher.sequences.rejected_stale);
    writer.u64(publisher.sequences.counter_resets);
    writer.u64(publisher.sequences.counter_wraps);
    writer.u64(publisher.sequences.sequence_gaps);
    writer.u64(publisher.accepted_events);
    writer.u64(publisher.accepted_batches);
    writer.u64(publisher.accepted_counters);
    writer.u64(publisher.rejected_events);
  }

  writer.u32(static_cast<std::uint32_t>(snapshot.processors().size()));
  for (const ProcessorRecord& record : snapshot.processors()) {
    const Status status = write_processor(writer, record);
    if (!status.ok()) return status;
  }
  writer.u32(static_cast<std::uint32_t>(snapshot.accelerators().size()));
  for (const AcceleratorRecord& record : snapshot.accelerators()) {
    const Status status = write_accelerator(writer, record);
    if (!status.ok()) return status;
  }
  writer.u32(static_cast<std::uint32_t>(snapshot.memory_domains().size()));
  for (const MemoryDomainRecord& record : snapshot.memory_domains()) {
    const Status status = write_memory_domain(writer, record);
    if (!status.ok()) return status;
  }
  writer.u32(static_cast<std::uint32_t>(snapshot.coherence_domains().size()));
  for (const CoherenceDomainRecord& record : snapshot.coherence_domains()) {
    const Status status = write_coherence_domain(writer, record);
    if (!status.ok()) return status;
  }
  writer.u32(static_cast<std::uint32_t>(snapshot.regions().size()));
  for (const RegionRecord& record : snapshot.regions()) {
    const Status status = write_region(writer, record);
    if (!status.ok()) return status;
  }

  const std::size_t wire_bucket_limit = kMaxWireAggregateBuckets;
  const std::size_t held = snapshot.aggregates().buckets().size();
  const std::size_t bucket_count = held > wire_bucket_limit ? wire_bucket_limit : held;
  writer.u32(static_cast<std::uint32_t>(held));
  writer.u32(static_cast<std::uint32_t>(bucket_count));
  std::size_t written = 0;
  for (const auto& entry : snapshot.aggregates().buckets()) {
    if (written >= bucket_count) {
      break;
    }
    writer.u8(static_cast<std::uint8_t>(entry.first.dimension));
    Status status = writer.text(entry.first.value, kMaxTextAnnotation);
    if (!status.ok()) return status;
    writer.u64(entry.second.observations);
    writer.u64(entry.second.bytes);
    writer.u64(entry.second.remote_accesses);
    writer.u64(entry.second.remote_reads);
    writer.u64(entry.second.remote_writes);
    writer.u64(entry.second.invalidations);
    writer.u64(entry.second.ownership_transfers);
    writer.u64(entry.second.retries);
    writer.u64(entry.second.conflicts);
    writer.u64(entry.second.writebacks);
    writer.i64(entry.second.first_timestamp_ns);
    writer.i64(entry.second.last_timestamp_ns);
    writer.u8(entry.second.has_timestamps);
    writer.u8(entry.second.saturated);
    writer.u8(static_cast<std::uint8_t>(entry.second.weakest_precision));
    writer.u8(static_cast<std::uint8_t>(entry.second.reality()));
    for (std::uint64_t count : entry.second.precision_counts) {
      writer.u64(count);
    }
    for (std::uint64_t count : entry.second.provenance_counts) {
      writer.u64(count);
    }
    ++written;
  }

  writer.u32(static_cast<std::uint32_t>(snapshot.findings().size()));
  for (const Finding& finding : snapshot.findings()) {
    const Status status = write_finding(writer, finding);
    if (!status.ok()) return status;
  }

  writer.u32(static_cast<std::uint32_t>(snapshot.stale_evidence().size()));
  for (const StaleEvidenceRecord& record : snapshot.stale_evidence()) {
    writer.u8(static_cast<std::uint8_t>(record.kind));
    writer.u64(record.event_id.value());
    Status status = write_name_id(writer, record.publisher);
    if (!status.ok()) return status;
    writer.u64(record.publisher_boot.value());
    status = write_name_id(writer, record.region);
    if (!status.ok()) return status;
    writer.u8(static_cast<std::uint8_t>(record.event_type));
    writer.i64(record.observed_at_ns);
    status = writer.text(record.detail, 256);
    if (!status.ok()) return status;
  }

  writer.u32(static_cast<std::uint32_t>(snapshot.capabilities().size()));
  for (const Capability& capability : snapshot.capabilities()) {
    const Status status = write_capability(writer, capability);
    if (!status.ok()) return status;
  }

  const LossReport& loss = snapshot.loss().loss;
  writer.u64(loss.rejected_observations);
  writer.u64(loss.rejected_duplicates);
  writer.u64(loss.rejected_conflicting_duplicates);
  writer.u64(loss.rejected_stale_sequences);
  writer.u64(loss.rejected_unauthorized);
  writer.u64(loss.rejected_malformed);
  writer.u64(loss.rejected_overflow);
  writer.u64(loss.dropped_aggregate_updates);
  writer.u64(loss.dropped_journal_entries);
  writer.u64(loss.dropped_region_samples);
  writer.u64(loss.evicted_history_records);
  writer.u64(loss.missing_sequences);
  writer.u64(loss.backpressure_rejections);
  writer.u64(snapshot.loss().aggregate_saturations);
  return Status();
}

Result<SnapshotPtr> read_snapshot(PayloadReader& reader) {
  auto snapshot = std::make_shared<Snapshot>();
  Snapshot& out = *snapshot;
  std::uint64_t generation = 0;
  std::uint64_t coordinator = 0;
  std::uint64_t observation = 0;
  std::uint64_t topology = 0;
  std::int64_t captured = 0;
  std::uint8_t reality = 0;
  std::uint64_t historical_observations = 0;
  std::uint64_t history_epoch = 0;
  std::uint64_t historical_aggregates = 0;
  std::uint64_t history_records = 0;

  Status status = reader.u64(&generation);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.u64(&coordinator);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.u64(&observation);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.u64(&topology);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.i64(&captured);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = require_enum(reader, &reality, 3, "snapshot reality");
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.u64(&historical_observations);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.u64(&history_epoch);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.u64(&historical_aggregates);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.u64(&history_records);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());

  std::uint32_t publisher_count = 0;
  status = reader.u32(&publisher_count);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  if (publisher_count > Limits::kMaxPublisherCount) {
    return fail_as<SnapshotPtr>(ErrorCode::TooMany, "snapshot publisher count exceeds the bound");
  }
  std::vector<PublisherView> publishers;
  for (std::uint32_t i = 0; i < publisher_count; ++i) {
    PublisherView publisher;
    std::uint64_t boot = 0;
    std::uint64_t registered_epoch = 0;
    std::uint64_t evidence = 0;
    std::uint64_t sampling = 0;
    std::int64_t registered_at = 0;
    std::int64_t last_seen = 0;
    std::uint8_t provenance = 0;
    std::uint8_t publisher_reality = 0;
    std::uint8_t current = 0;
    std::uint8_t fenced = 0;
    std::uint8_t fence_reason = 0;
    std::uint8_t loaded = 0;
    status = read_name_id(reader, &publisher.id);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&boot);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = read_name_id(reader, &publisher.observer);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = read_name_id(reader, &publisher.node);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.text(&publisher.display_name, kMaxTextAnnotation);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = require_enum(reader, &provenance, 10, "publisher provenance");
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = require_enum(reader, &publisher_reality, 3, "publisher reality");
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&registered_epoch);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&evidence);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&sampling);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.i64(&registered_at);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.i64(&last_seen);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u8(&current);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u8(&fenced);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = require_enum(reader, &fence_reason, 6, "fence reason");
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.text(&publisher.fence_detail, kMaxTextAnnotation);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u8(&loaded);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    std::uint64_t high_watermark = 0;
    std::uint64_t next_expected = 0;
    status = reader.u64(&high_watermark);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&next_expected);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    publisher.boot = PublisherBootId{boot};
    publisher.provenance = static_cast<Provenance>(provenance);
    publisher.reality = static_cast<Reality>(publisher_reality);
    publisher.registered_epoch = CoordinatorEpoch{registered_epoch};
    publisher.evidence_generation = EvidenceGeneration{evidence};
    publisher.sampling_epoch = SamplingEpoch{sampling};
    publisher.registered_at_ns = static_cast<Nanos>(registered_at);
    publisher.last_seen_ns = static_cast<Nanos>(last_seen);
    publisher.current = current != 0;
    publisher.fenced = fenced != 0;
    publisher.fence_reason = static_cast<FenceReason>(fence_reason);
    publisher.loaded_from_state = loaded != 0;
    publisher.sequences.high_watermark = EventSequence{high_watermark};
    publisher.sequences.next_expected = EventSequence{next_expected};
    status = reader.u64(&publisher.sequences.accepted);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&publisher.sequences.duplicates);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&publisher.sequences.conflicting_duplicates);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&publisher.sequences.late_events);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&publisher.sequences.missing_events);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&publisher.sequences.rejected_stale);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&publisher.sequences.counter_resets);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&publisher.sequences.counter_wraps);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&publisher.sequences.sequence_gaps);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&publisher.accepted_events);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&publisher.accepted_batches);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&publisher.accepted_counters);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&publisher.rejected_events);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    publishers.push_back(std::move(publisher));
  }

  std::uint32_t count = 0;
  status = reader.u32(&count);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  if (count > Limits::kMaxProcessorCount) {
    return fail_as<SnapshotPtr>(ErrorCode::TooMany, "snapshot processor count exceeds the bound");
  }
  std::vector<ProcessorRecord> processors;
  for (std::uint32_t i = 0; i < count; ++i) {
    const Result<ProcessorRecord> record = read_processor(reader);
    if (!record.ok()) return Result<SnapshotPtr>(record.error());
    processors.push_back(record.value());
  }
  status = reader.u32(&count);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  if (count > Limits::kMaxAcceleratorCount) {
    return fail_as<SnapshotPtr>(ErrorCode::TooMany, "snapshot accelerator count exceeds bound");
  }
  std::vector<AcceleratorRecord> accelerators;
  for (std::uint32_t i = 0; i < count; ++i) {
    const Result<AcceleratorRecord> record = read_accelerator(reader);
    if (!record.ok()) return Result<SnapshotPtr>(record.error());
    accelerators.push_back(record.value());
  }
  status = reader.u32(&count);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  if (count > Limits::kMaxMemoryDomainCount) {
    return fail_as<SnapshotPtr>(ErrorCode::TooMany, "snapshot memory domain count exceeds bound");
  }
  std::vector<MemoryDomainRecord> domains;
  for (std::uint32_t i = 0; i < count; ++i) {
    const Result<MemoryDomainRecord> record = read_memory_domain(reader);
    if (!record.ok()) return Result<SnapshotPtr>(record.error());
    domains.push_back(record.value());
  }
  status = reader.u32(&count);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  if (count > Limits::kMaxCoherenceDomainCount) {
    return fail_as<SnapshotPtr>(ErrorCode::TooMany,
                                "snapshot coherence domain count exceeds bound");
  }
  std::vector<CoherenceDomainRecord> coherence_domains;
  for (std::uint32_t i = 0; i < count; ++i) {
    const Result<CoherenceDomainRecord> record = read_coherence_domain(reader);
    if (!record.ok()) return Result<SnapshotPtr>(record.error());
    coherence_domains.push_back(record.value());
  }
  status = reader.u32(&count);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  if (count > Limits::kMaxRegionCount) {
    return fail_as<SnapshotPtr>(ErrorCode::TooMany, "snapshot region count exceeds the bound");
  }
  std::vector<RegionRecord> regions;
  for (std::uint32_t i = 0; i < count; ++i) {
    const Result<RegionRecord> record = read_region(reader);
    if (!record.ok()) return Result<SnapshotPtr>(record.error());
    regions.push_back(record.value());
  }

  std::uint32_t buckets_held = 0;
  status = reader.u32(&buckets_held);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  std::uint32_t bucket_count = 0;
  status = reader.u32(&bucket_count);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  if (bucket_count > kMaxWireAggregateBuckets || bucket_count > buckets_held) {
    return fail_as<SnapshotPtr>(ErrorCode::TooMany, "snapshot bucket count exceeds the bound");
  }
  std::map<AggregateKey, AggregateValue> buckets;
  for (std::uint32_t i = 0; i < bucket_count; ++i) {
    AggregateKey key;
    AggregateValue value;
    std::uint8_t dimension = 0;
    std::uint8_t precision = 0;
    std::uint8_t bucket_reality = 0;
    status = require_enum(reader, &dimension, kAggregateDimensionCount,
                          "aggregate dimension");
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.text(&key.value, kMaxTextAnnotation);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&value.observations);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&value.bytes);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&value.remote_accesses);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&value.remote_reads);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&value.remote_writes);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&value.invalidations);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&value.ownership_transfers);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&value.retries);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&value.conflicts);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&value.writebacks);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.i64(&value.first_timestamp_ns);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.i64(&value.last_timestamp_ns);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    std::uint8_t has_timestamps = 0;
    std::uint8_t saturated = 0;
    status = reader.u8(&has_timestamps);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u8(&saturated);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = require_enum(reader, &precision, 7, "aggregate precision");
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = require_enum(reader, &bucket_reality, 3, "aggregate reality");
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    for (std::uint64_t& precision_slot : value.precision_counts) {
      status = reader.u64(&precision_slot);
      if (!status.ok()) return Result<SnapshotPtr>(status.error());
    }
    for (std::uint64_t& provenance_slot : value.provenance_counts) {
      status = reader.u64(&provenance_slot);
      if (!status.ok()) return Result<SnapshotPtr>(status.error());
    }
    key.dimension = static_cast<AggregateDimension>(dimension);
    value.has_timestamps = has_timestamps != 0;
    value.saturated = saturated != 0;
    value.weakest_precision = static_cast<Precision>(precision);
    value.has_observations = value.observations != 0;
    value.synthetic_contribution = bucket_reality == static_cast<std::uint8_t>(Reality::Synthetic);
    value.mixed_contribution = bucket_reality == static_cast<std::uint8_t>(Reality::Mixed);
    buckets[key] = value;
  }

  status = reader.u32(&count);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  if (count > Limits::kMaxFindings) {
    return fail_as<SnapshotPtr>(ErrorCode::TooMany, "snapshot finding count exceeds the bound");
  }
  std::vector<Finding> findings;
  for (std::uint32_t i = 0; i < count; ++i) {
    const Result<Finding> finding = read_finding(reader);
    if (!finding.ok()) return Result<SnapshotPtr>(finding.error());
    findings.push_back(finding.value());
  }

  status = reader.u32(&count);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  if (count > Limits::kMaxFindings) {
    return fail_as<SnapshotPtr>(ErrorCode::TooMany, "snapshot stale count exceeds the bound");
  }
  std::vector<StaleEvidenceRecord> stale;
  for (std::uint32_t i = 0; i < count; ++i) {
    StaleEvidenceRecord record;
    std::uint8_t kind = 0;
    std::uint8_t event_type = 0;
    std::uint64_t event_id = 0;
    std::uint64_t boot = 0;
    std::int64_t observed_at = 0;
    status = require_enum(reader, &kind, 7, "stale evidence kind");
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&event_id);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = read_name_id(reader, &record.publisher);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.u64(&boot);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = read_name_id(reader, &record.region);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = require_enum(reader, &event_type, kEventTypeCount, "stale event type");
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.i64(&observed_at);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    status = reader.text(&record.detail, 256);
    if (!status.ok()) return Result<SnapshotPtr>(status.error());
    record.kind = static_cast<StaleEvidenceRecord::Kind>(kind);
    record.event_id = CoherenceEventId{event_id};
    record.publisher_boot = PublisherBootId{boot};
    record.event_type = static_cast<EventType>(event_type);
    record.observed_at_ns = static_cast<Nanos>(observed_at);
    stale.push_back(std::move(record));
  }

  status = reader.u32(&count);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  if (count > 256) {
    return fail_as<SnapshotPtr>(ErrorCode::TooMany, "snapshot capability count exceeds bound");
  }
  std::vector<Capability> capabilities;
  for (std::uint32_t i = 0; i < count; ++i) {
    const Result<Capability> capability = read_capability(reader);
    if (!capability.ok()) return Result<SnapshotPtr>(capability.error());
    capabilities.push_back(capability.value());
  }

  LossReport loss;
  status = reader.u64(&loss.rejected_observations);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.u64(&loss.rejected_duplicates);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.u64(&loss.rejected_conflicting_duplicates);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.u64(&loss.rejected_stale_sequences);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.u64(&loss.rejected_unauthorized);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.u64(&loss.rejected_malformed);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.u64(&loss.rejected_overflow);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.u64(&loss.dropped_aggregate_updates);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.u64(&loss.dropped_journal_entries);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.u64(&loss.dropped_region_samples);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.u64(&loss.evicted_history_records);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.u64(&loss.missing_sequences);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  status = reader.u64(&loss.backpressure_rejections);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());
  std::uint64_t saturations = 0;
  status = reader.u64(&saturations);
  if (!status.ok()) return Result<SnapshotPtr>(status.error());

  SnapshotAssembler::assign(out, generation, coordinator, observation, topology, captured,
                            static_cast<Reality>(reality), std::move(publishers),
                            std::move(processors), std::move(accelerators), std::move(domains),
                            std::move(coherence_domains), std::move(regions), std::move(buckets),
                            buckets_held, buckets_held - bucket_count, std::move(findings),
                            std::move(stale), std::move(capabilities), loss, saturations,
                            historical_observations, history_epoch, historical_aggregates,
                            history_records);
  return Result<SnapshotPtr>(std::move(snapshot));
}

Status write_ack(PayloadWriter& writer, const AckPayload& ack) {
  writer.u16(static_cast<std::uint16_t>(ack.status));
  writer.u64(ack.coordinator_epoch.value());
  writer.u64(ack.observation_epoch.value());
  Status status = writer.text(ack.detail, 256);
  if (!status.ok()) return status;
  writer.u64(ack.accepted);
  writer.u64(ack.rejected);
  writer.u64(ack.duplicates);
  writer.u64(ack.late);
  writer.u64(ack.missing);
  writer.u64(ack.produced_delta);
  writer.u64(ack.delta);
  writer.u64(ack.discontinuity);
  return Status();
}

Result<AckPayload> read_ack(PayloadReader& reader) {
  AckPayload ack;
  std::uint16_t status_value = 0;
  std::uint64_t coordinator = 0;
  std::uint64_t observation = 0;
  Status status = reader.u16(&status_value);
  if (!status.ok()) return Result<AckPayload>(status.error());
  if (status_value > static_cast<std::uint16_t>(AckStatus::InternalError)) {
    return fail_as<AckPayload>(ErrorCode::InvalidArgument, "ack status out of range");
  }
  status = reader.u64(&coordinator);
  if (!status.ok()) return Result<AckPayload>(status.error());
  status = reader.u64(&observation);
  if (!status.ok()) return Result<AckPayload>(status.error());
  status = reader.text(&ack.detail, 256);
  if (!status.ok()) return Result<AckPayload>(status.error());
  status = reader.u64(&ack.accepted);
  if (!status.ok()) return Result<AckPayload>(status.error());
  status = reader.u64(&ack.rejected);
  if (!status.ok()) return Result<AckPayload>(status.error());
  status = reader.u64(&ack.duplicates);
  if (!status.ok()) return Result<AckPayload>(status.error());
  status = reader.u64(&ack.late);
  if (!status.ok()) return Result<AckPayload>(status.error());
  status = reader.u64(&ack.missing);
  if (!status.ok()) return Result<AckPayload>(status.error());
  status = reader.u64(&ack.produced_delta);
  if (!status.ok()) return Result<AckPayload>(status.error());
  status = reader.u64(&ack.delta);
  if (!status.ok()) return Result<AckPayload>(status.error());
  status = reader.u64(&ack.discontinuity);
  if (!status.ok()) return Result<AckPayload>(status.error());
  ack.status = static_cast<AckStatus>(status_value);
  ack.coordinator_epoch = CoordinatorEpoch{coordinator};
  ack.observation_epoch = ObservationEpoch{observation};
  return Result<AckPayload>(std::move(ack));
}

}  // namespace sol::coherence::detail