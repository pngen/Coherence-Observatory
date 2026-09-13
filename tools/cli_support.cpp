// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "cli_support.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include "coherence/explanation.hpp"

namespace coherence_cli {

namespace {

using sol::coherence::Finding;
using sol::coherence::Snapshot;

std::string quote_json(const std::string& text) {
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
          out.push_back('?');
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  return out;
}

char decimal_of(std::uint64_t value, int position) {
  static const char digits[] = "0123456789";
  std::uint64_t divisor = 1;
  for (int i = 0; i < position; ++i) {
    divisor *= 10;
  }
  return digits[(value / divisor) % 10];
}

}  // namespace

sol::coherence::Status split_endpoint(const std::string& endpoint, std::string* host,
                                 std::uint16_t* port) {
  const std::size_t colon = endpoint.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= endpoint.size()) {
    return sol::coherence::fail(sol::coherence::ErrorCode::InvalidArgument,
                           "endpoint must be host:port", endpoint);
  }
  *host = endpoint.substr(0, colon);
  const std::string port_text = endpoint.substr(colon + 1);
  std::uint32_t value = 0;
  for (char c : port_text) {
    if (c < '0' || c > '9') {
      return sol::coherence::fail(sol::coherence::ErrorCode::InvalidArgument, "port is not numeric",
                             port_text);
    }
    value = value * 10u + static_cast<std::uint32_t>(c - '0');
    if (value > 65535u) {
      return sol::coherence::fail(sol::coherence::ErrorCode::OutOfRange, "port out of range", port_text);
    }
  }
  if (value == 0) {
    return sol::coherence::fail(sol::coherence::ErrorCode::OutOfRange, "port must be non-zero", port_text);
  }
  *port = static_cast<std::uint16_t>(value);
  return sol::coherence::Status();
}

sol::coherence::Status parse_source(const std::vector<std::string>& arguments, Source* source,
                               std::vector<std::string>* positional) {
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    const std::string& argument = arguments[i];
    if (argument == "--endpoint") {
      if (i + 1 >= arguments.size()) {
        return sol::coherence::fail(sol::coherence::ErrorCode::InvalidArgument,
                               "--endpoint requires a value");
      }
      source->endpoint = arguments[++i];
    } else if (argument == "--state") {
      if (i + 1 >= arguments.size()) {
        return sol::coherence::fail(sol::coherence::ErrorCode::InvalidArgument,
                               "--state requires a value");
      }
      source->state_path = arguments[++i];
      source->from_state = true;
    } else if (positional != nullptr) {
      positional->push_back(argument);
    }
  }
  if (source->endpoint.empty() && source->state_path.empty()) {
    return sol::coherence::fail(sol::coherence::ErrorCode::InvalidArgument,
                           "either --endpoint host:port or --state <file> is required");
  }
  return sol::coherence::Status();
}

struct Reader::Impl {
  Source source;
  bool opened = false;
  std::unique_ptr<sol::coherence::Observatory> local;
  std::unique_ptr<sol::coherence::ObservationClient> remote;
};

Reader::Reader() : impl_(std::make_unique<Impl>()) {}
Reader::~Reader() = default;

sol::coherence::Status Reader::open(const Source& source) {
  impl_->source = source;
  if (source.from_state) {
    sol::coherence::ObservatoryOptions options;
    impl_->local = std::make_unique<sol::coherence::Observatory>(options);
    const sol::coherence::Result<sol::coherence::PersistenceReport> loaded =
        impl_->local->load_state(source.state_path);
    if (!loaded.ok()) {
      return sol::coherence::Status(loaded.error());
    }
  } else {
    std::string host;
    std::uint16_t port = 0;
    const sol::coherence::Status split = split_endpoint(source.endpoint, &host, &port);
    if (!split.ok()) {
      return split;
    }
    sol::coherence::ClientOptions client_options;
    client_options.host = host;
    client_options.port = port;
    client_options.client_name = "cohobs-inspect";
    impl_->remote = std::make_unique<sol::coherence::ObservationClient>();
    const sol::coherence::Status connected = impl_->remote->connect(client_options);
    if (!connected.ok()) {
      return connected;
    }
  }
  impl_->opened = true;
  return sol::coherence::Status();
}

bool Reader::open() const noexcept { return impl_->opened; }

sol::coherence::Result<sol::coherence::SnapshotPtr> Reader::snapshot(
    const sol::coherence::FindingsOptions& options) const {
  if (!impl_->opened) {
    return sol::coherence::fail_as<sol::coherence::SnapshotPtr>(sol::coherence::ErrorCode::Shutdown,
                                                      "reader is not open");
  }
  if (impl_->local != nullptr) {
    return sol::coherence::Result<sol::coherence::SnapshotPtr>(impl_->local->snapshot(options));
  }
  return impl_->remote->query_snapshot(options);
}

sol::coherence::Result<std::vector<Finding>> Reader::findings(
    const sol::coherence::FindingsOptions& options) const {
  if (!impl_->opened) {
    return sol::coherence::fail_as<std::vector<Finding>>(sol::coherence::ErrorCode::Shutdown,
                                                    "reader is not open");
  }
  if (impl_->local != nullptr) {
    return impl_->local->findings(options);
  }
  return impl_->remote->query_findings(options);
}

sol::coherence::Result<sol::coherence::AttributionResult> Reader::attribution(
    const sol::coherence::MemoryRegionId& region) const {
  if (!impl_->opened) {
    return sol::coherence::fail_as<sol::coherence::AttributionResult>(sol::coherence::ErrorCode::Shutdown,
                                                            "reader is not open");
  }
  if (impl_->local != nullptr) {
    return impl_->local->attribute_region(region);
  }
  return impl_->remote->query_attribution(region);
}

sol::coherence::Observatory* Reader::local_observatory() noexcept {
  return impl_->local.get();
}

struct AdminReader::Impl {
  std::unique_ptr<sol::coherence::ObservatoryAdminClient> client;
  bool opened = false;
};

AdminReader::AdminReader() : impl_(std::make_unique<Impl>()) {}
AdminReader::~AdminReader() = default;

sol::coherence::Status AdminReader::open(const Source& source) {
  if (source.from_state) {
    return sol::coherence::fail(sol::coherence::ErrorCode::UnsupportedCapability,
                           "administrative commands require a live coordinator; a state file "
                           "is read-only");
  }
  std::string host;
  std::uint16_t port = 0;
  const sol::coherence::Status split = split_endpoint(source.endpoint, &host, &port);
  if (!split.ok()) {
    return split;
  }
  sol::coherence::ClientOptions client_options;
  client_options.host = host;
  client_options.port = port;
  client_options.client_name = "cohobs-admin";
  impl_->client = std::make_unique<sol::coherence::ObservatoryAdminClient>();
  const sol::coherence::Status connected = impl_->client->connect(client_options);
  if (!connected.ok()) {
    return connected;
  }
  impl_->opened = true;
  return sol::coherence::Status();
}

bool AdminReader::open() const noexcept { return impl_->opened; }

sol::coherence::ObservatoryAdminClient& AdminReader::client() { return *impl_->client; }

// ---- Rendering ---------------------------------------------------------

std::string bool_text(bool value) { return value ? "true" : "false"; }

std::string bytes_text(std::uint64_t value) {
  static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double scaled = static_cast<double>(value);
  std::size_t unit = 0;
  while (scaled >= 1024.0 && unit + 1 < 5) {
    scaled /= 1024.0;
    ++unit;
  }
  std::array<char, 64> buffer{};
  if (unit == 0) {
    std::snprintf(buffer.data(), buffer.size(), "%llu %s",
                  static_cast<unsigned long long>(value), units[unit]);
  } else {
    std::snprintf(buffer.data(), buffer.size(), "%.2f %s", scaled, units[unit]);
  }
  return std::string(buffer.data());
}

std::string rate_text(double value) {
  std::array<char, 64> buffer{};
  std::snprintf(buffer.data(), buffer.size(), "%.2f/s", value);
  return std::string(buffer.data());
}

std::string nanos_text(std::int64_t value) {
  return sol::coherence::format_duration_ns(value);
}

std::string precision_breakdown(const sol::coherence::AggregateValue& value) {
  std::string out;
  static const sol::coherence::Precision order[] = {
      sol::coherence::Precision::ExactEvent, sol::coherence::Precision::ExactCounterDelta,
      sol::coherence::Precision::SampledEvent, sol::coherence::Precision::AggregatedCounter,
      sol::coherence::Precision::Derived, sol::coherence::Precision::Inferred,
      sol::coherence::Precision::Unknown};
  for (sol::coherence::Precision precision : order) {
    const std::size_t index = sol::coherence::precision_rank(precision);
    const std::uint64_t count = value.precision_counts[index];
    if (count == 0) {
      continue;
    }
    if (!out.empty()) {
      out.append(" ");
    }
    out.append(sol::coherence::to_string(precision));
    out.push_back('=');
    out.append(std::to_string(count));
  }
  return out.empty() ? std::string("-") : out;
}

std::string reality_breakdown(const sol::coherence::AggregateValue& value) {
  return std::string(sol::coherence::to_string(value.reality()));
}

std::string render_table(const std::vector<std::string>& headers,
                         const std::vector<std::vector<std::string>>& rows) {
  std::vector<std::size_t> widths(headers.size(), 0);
  for (std::size_t i = 0; i < headers.size(); ++i) {
    widths[i] = headers[i].size();
  }
  for (const std::vector<std::string>& row : rows) {
    for (std::size_t i = 0; i < row.size() && i < widths.size(); ++i) {
      widths[i] = std::max(widths[i], row[i].size());
    }
  }
  std::string out;
  auto emit = [&out, &widths](const std::vector<std::string>& row) {
    for (std::size_t i = 0; i < widths.size(); ++i) {
      const std::string cell = i < row.size() ? row[i] : std::string();
      out.append(cell);
      if (i + 1 < widths.size()) {
        out.append(widths[i] - cell.size() + 2, ' ');
      }
    }
    out.push_back('\n');
  };
  emit(headers);
  std::vector<std::string> separator;
  separator.reserve(widths.size());
  for (std::size_t width : widths) {
    separator.push_back(std::string(width, '-'));
  }
  emit(separator);
  for (const std::vector<std::string>& row : rows) {
    emit(row);
  }
  return out;
}

std::string render_findings(const std::vector<Finding>& findings, bool explain) {
  std::vector<std::vector<std::string>> rows;
  for (const Finding& finding : findings) {
    rows.push_back({sol::coherence::to_string(finding.kind).data(), finding.subject_kind,
                    finding.subject,
                    std::string(sol::coherence::to_string(finding.precision)),
                    std::string(sol::coherence::to_string(finding.reality)),
                    std::to_string(finding.reasons.size()),
                    finding.historical ? "historical" : "current"});
  }
  std::string out = render_table({"kind", "subject_kind", "subject", "precision", "reality",
                                  "reasons", "scope"},
                                 rows);
  if (!explain) {
    return out;
  }
  for (const Finding& finding : findings) {
    out.append("\n");
    out.append(sol::coherence::render_text(sol::coherence::explain(finding)));
  }
  return out;
}

std::string render_capabilities(const std::vector<sol::coherence::Capability>& capabilities) {
  std::vector<std::vector<std::string>> rows;
  for (const sol::coherence::Capability& capability : capabilities) {
    rows.push_back({capability.key, std::string(sol::coherence::to_string(capability.status)),
                    capability.mechanism, capability.detail});
  }
  return render_table({"capability", "status", "mechanism", "detail"}, rows);
}

std::string render_snapshot_json(const Snapshot& snapshot) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"snapshot_generation\": " << snapshot.generation().value() << ",\n";
  out << "  \"coordinator_epoch\": " << snapshot.coordinator_epoch().value() << ",\n";
  out << "  \"observation_epoch\": " << snapshot.observation_epoch().value() << ",\n";
  out << "  \"topology_generation\": " << snapshot.topology_generation().value() << ",\n";
  out << "  \"captured_at_ns\": " << snapshot.captured_at_ns() << ",\n";
  out << "  \"reality\": \"" << sol::coherence::to_string(snapshot.reality()) << "\",\n";
  out << "  \"historical_observation_count\": " << snapshot.historical_observation_count()
      << ",\n";
  out << "  \"history_source_epoch\": " << snapshot.history_source_epoch().value() << ",\n";
  out << "  \"publishers\": [\n";
  for (std::size_t i = 0; i < snapshot.publishers().size(); ++i) {
    const sol::coherence::PublisherView& publisher = snapshot.publishers()[i];
    out << "    {\"id\": \"" << quote_json(publisher.id.str()) << "\", \"boot\": "
        << publisher.boot.value() << ", \"current\": " << bool_text(publisher.current)
        << ", \"fenced\": " << bool_text(publisher.fenced) << ", \"fence_reason\": \""
        << sol::coherence::to_string(publisher.fence_reason) << "\", \"provenance\": \""
        << sol::coherence::to_string(publisher.provenance) << "\", \"reality\": \""
        << sol::coherence::to_string(publisher.reality) << "\", \"accepted\": "
        << publisher.accepted_events << ", \"high_watermark\": "
        << publisher.sequences.high_watermark.value() << "}" << (i + 1 < snapshot.publishers().size() ? "," : "")
        << "\n";
  }
  out << "  ],\n";
  out << "  \"regions\": [\n";
  for (std::size_t i = 0; i < snapshot.regions().size(); ++i) {
    const sol::coherence::RegionRecord& region = snapshot.regions()[i];
    out << "    {\"id\": \"" << quote_json(region.id.str()) << "\", \"generation\": "
        << region.generation.value() << ", \"memory_domain\": \""
        << quote_json(region.memory_domain.str()) << "\", \"retired\": "
        << bool_text(region.retired) << ", \"owner\": \"" << quote_json(region.owner) << "\""
        << "}" << (i + 1 < snapshot.regions().size() ? "," : "") << "\n";
  }
  out << "  ],\n";
  out << "  \"aggregates\": [\n";
  std::size_t index = 0;
  for (const auto& entry : snapshot.aggregates().buckets()) {
    out << "    {\"dimension\": \"" << sol::coherence::to_string(entry.first.dimension)
        << "\", \"value\": \"" << quote_json(entry.first.value) << "\", \"observations\": "
        << entry.second.observations << ", \"bytes\": " << entry.second.bytes
        << ", \"remote_accesses\": " << entry.second.remote_accesses
        << ", \"invalidations\": " << entry.second.invalidations
        << ", \"ownership_transfers\": " << entry.second.ownership_transfers
        << ", \"reality\": \"" << sol::coherence::to_string(entry.second.reality())
        << "\", \"weakest_precision\": \""
        << sol::coherence::to_string(entry.second.weakest_precision) << "\"}"
        << (index + 1 < snapshot.aggregates().buckets().size() ? "," : "") << "\n";
    ++index;
  }
  out << "  ],\n";
  out << "  \"findings\": [\n";
  for (std::size_t i = 0; i < snapshot.findings().size(); ++i) {
    const Finding& finding = snapshot.findings()[i];
    out << "    {\"kind\": \"" << sol::coherence::to_string(finding.kind) << "\", \"subject\": \""
        << quote_json(finding.subject) << "\", \"precision\": \""
        << sol::coherence::to_string(finding.precision) << "\", \"reality\": \""
        << sol::coherence::to_string(finding.reality) << "\""
        << "}" << (i + 1 < snapshot.findings().size() ? "," : "") << "\n";
  }
  out << "  ],\n";
  out << "  \"stale_evidence\": " << snapshot.stale_evidence().size() << ",\n";
  out << "  \"loss\": {\"rejected\": "
      << snapshot.loss().loss.total_rejected() << ", \"missing_sequences\": "
      << snapshot.loss().loss.missing_sequences << "},\n";
  out << "  \"capabilities\": [\n";
  for (std::size_t i = 0; i < snapshot.capabilities().size(); ++i) {
    const sol::coherence::Capability& capability = snapshot.capabilities()[i];
    out << "    {\"key\": \"" << quote_json(capability.key) << "\", \"status\": \""
        << sol::coherence::to_string(capability.status) << "\", \"mechanism\": \""
        << quote_json(capability.mechanism) << "\"}"
        << (i + 1 < snapshot.capabilities().size() ? "," : "") << "\n";
  }
  out << "  ]\n}\n";
  return out.str();
}

}  // namespace coherence_cli