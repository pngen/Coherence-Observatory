// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Provenance and reality classification.
//
// REAL evidence comes from a source that genuinely observes the phenomenon.
// SYNTHETIC evidence is produced by a simulator that knows the truth by
// construction.  Neither may ever be relabelled as the other.

#ifndef COHERENCE_PROVENANCE_HPP
#define COHERENCE_PROVENANCE_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "coherence/error.hpp"
#include "coherence/export.hpp"

namespace sol::coherence {

/// Where an observation came from.
enum class Provenance : std::uint8_t {
  Unknown = 0,
  HardwarePerformanceCounter = 1,
  VendorTelemetryApi = 2,
  OsTelemetry = 3,
  RuntimeInstrumentation = 4,
  ApplicationInstrumentation = 5,
  FabricProvidedState = 6,
  SyntheticBackend = 7,
  ImportedTrace = 8,
  DerivedAggregation = 9,
};

COHERENCE_API std::string_view to_string(Provenance provenance) noexcept;
COHERENCE_API Result<Provenance> parse_provenance(std::string_view text);

/// True when the provenance denotes a real observation of real hardware.
COHERENCE_API bool is_real_provenance(Provenance provenance) noexcept;

/// True when the provenance denotes simulated evidence.
COHERENCE_API bool is_synthetic_provenance(Provenance provenance) noexcept;

/// Reality class of a piece of evidence or of a derived result.
enum class Reality : std::uint8_t {
  /// Every contributing observation was REAL.
  Real = 0,
  /// Every contributing observation was SYNTHETIC.
  Synthetic = 1,
  /// Contributions were mixed; the result is at best as good as its weakest
  /// contributor and is never reported as REAL.
  Mixed = 2,
};

COHERENCE_API std::string_view to_string(Reality reality) noexcept;

/// Reality of a single provenance class.
COHERENCE_API Reality reality_of(Provenance provenance) noexcept;

/// Combines realities conservatively: Mixed dominates, otherwise agreement.
COHERENCE_API Reality combine_reality(Reality a, Reality b) noexcept;

/// Classification of a hardware-facing capability on this host.
enum class CapabilityStatus : std::uint8_t {
  /// Genuinely observable here and actually observed.
  Real = 0,
  /// Simulated because the host cannot expose it.
  Synthetic = 1,
  /// Not available on this host and not simulated; claimed by nobody.
  Unsupported = 2,
};

COHERENCE_API std::string_view to_string(CapabilityStatus status) noexcept;
COHERENCE_API Result<CapabilityStatus> parse_capability_status(std::string_view text);

/// One classified capability with the evidence that justifies the class.
struct COHERENCE_API Capability {
  /// Stable capability key, e.g. "cpu.numa_topology".
  std::string key;
  CapabilityStatus status = CapabilityStatus::Unsupported;
  /// Short human-readable statement of what is (not) observable and why.
  std::string detail;
  /// Mechanism actually used, e.g. "GetLogicalProcessorInformationEx".
  std::string mechanism;
};

/// Deterministic ordering by key then status; duplicate keys are removed.
///
/// Under that ordering the retained record for a duplicated key is the one
/// with the lowest status ordinal, i.e. REAL before SYNTHETIC before
/// UNSUPPORTED.  Callers that must not lose a stronger classification should
/// not register conflicting entries for one key.
COHERENCE_API void sort_capabilities(std::vector<Capability>& capabilities);

/// Standard explanation text for a capability status.
COHERENCE_API std::string_view capability_status_explanation(CapabilityStatus status);

}  // namespace sol::coherence

#endif  // COHERENCE_PROVENANCE_HPP
