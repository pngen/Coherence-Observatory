// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/provenance.hpp"

#include <algorithm>
#include <array>

#include "text_util.hpp"

namespace sol::coherence {
namespace {

constexpr std::array<std::string_view, 10> kProvenanceNames = {
    "UNKNOWN", "HARDWARE_PERFORMANCE_COUNTER", "VENDOR_TELEMETRY_API", "OS_TELEMETRY",
    "RUNTIME_INSTRUMENTATION", "APPLICATION_INSTRUMENTATION", "FABRIC_PROVIDED_STATE",
    "SYNTHETIC_BACKEND", "IMPORTED_TRACE", "DERIVED_AGGREGATION"};

constexpr std::array<std::string_view, 3> kRealityNames = {"REAL", "SYNTHETIC", "MIXED"};

constexpr std::array<std::string_view, 3> kCapabilityStatusNames = {
    "REAL", "SYNTHETIC", "UNSUPPORTED"};

std::string_view capability_detail(CapabilityStatus status) noexcept {
  switch (status) {
    case CapabilityStatus::Real: return "observable on this host and observed";
    case CapabilityStatus::Synthetic: return "not observable on this host; proven synthetically";
    case CapabilityStatus::Unsupported: return "not observable on this host; not simulated";
  }
  return "unknown";
}

}  // namespace

std::string_view to_string(Provenance provenance) noexcept {
  const auto index = static_cast<std::size_t>(provenance);
  return index < kProvenanceNames.size() ? kProvenanceNames[index] : std::string_view("UNKNOWN");
}

Result<Provenance> parse_provenance(std::string_view text) {
  for (std::size_t i = 0; i < kProvenanceNames.size(); ++i) {
    if (detail::iequals(kProvenanceNames[i], text)) {
      return Result<Provenance>(static_cast<Provenance>(i));
    }
  }
  return Result<Provenance>(Error(ErrorCode::InvalidArgument, "unknown provenance",
                                  std::string(text)));
}

bool is_real_provenance(Provenance provenance) noexcept {
  switch (provenance) {
    case Provenance::HardwarePerformanceCounter:
    case Provenance::VendorTelemetryApi:
    case Provenance::OsTelemetry:
    case Provenance::RuntimeInstrumentation:
    case Provenance::ApplicationInstrumentation:
    case Provenance::FabricProvidedState:
      return true;
    default:
      return false;
  }
}

bool is_synthetic_provenance(Provenance provenance) noexcept {
  return provenance == Provenance::SyntheticBackend;
}

std::string_view to_string(Reality reality) noexcept {
  const auto index = static_cast<std::size_t>(reality);
  return index < kRealityNames.size() ? kRealityNames[index] : std::string_view("MIXED");
}

Reality reality_of(Provenance provenance) noexcept {
  if (is_synthetic_provenance(provenance)) {
    return Reality::Synthetic;
  }
  if (is_real_provenance(provenance)) {
    return Reality::Real;
  }
  // Imported traces, derived aggregations and unknown provenance are never
  // promoted to REAL: their underlying source is not established here.
  return Reality::Mixed;
}

Reality combine_reality(Reality a, Reality b) noexcept {
  if (a == b) {
    return a;
  }
  return Reality::Mixed;
}

std::string_view to_string(CapabilityStatus status) noexcept {
  const auto index = static_cast<std::size_t>(status);
  return index < kCapabilityStatusNames.size() ? kCapabilityStatusNames[index]
                                               : std::string_view("UNSUPPORTED");
}

Result<CapabilityStatus> parse_capability_status(std::string_view text) {
  for (std::size_t i = 0; i < kCapabilityStatusNames.size(); ++i) {
    if (detail::iequals(kCapabilityStatusNames[i], text)) {
      return Result<CapabilityStatus>(static_cast<CapabilityStatus>(i));
    }
  }
  return Result<CapabilityStatus>(
      Error(ErrorCode::InvalidArgument, "unknown capability status", std::string(text)));
}

void sort_capabilities(std::vector<Capability>& capabilities) {
  std::sort(capabilities.begin(), capabilities.end(),
            [](const Capability& a, const Capability& b) {
              if (a.key != b.key) {
                return a.key < b.key;
              }
              return static_cast<int>(a.status) < static_cast<int>(b.status);
            });
  capabilities.erase(
      std::unique(capabilities.begin(), capabilities.end(),
                  [](const Capability& a, const Capability& b) { return a.key == b.key; }),
      capabilities.end());
}

std::string_view capability_status_explanation(CapabilityStatus status) {
  return capability_detail(status);
}

}  // namespace sol::coherence
