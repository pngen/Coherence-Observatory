// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Imported-trace backend.
//
// Imports the canonical Coherence Observatory trace text format.  Everything
// imported keeps Provenance::ImportedTrace, so imported evidence can never be
// reported as REAL hardware evidence.

#ifndef COHERENCE_BACKENDS_IMPORTED_TRACE_HPP
#define COHERENCE_BACKENDS_IMPORTED_TRACE_HPP

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "coherence/backend.hpp"

namespace sol::coherence {

/// Canonical trace header line.
inline constexpr std::string_view kTraceMagic = "COBS-TRACE 1";

/// One diagnostics entry produced while importing.
struct COHERENCE_API TraceImportDiagnostic {
  std::size_t line = 0;
  std::string message;
};

/// Summary of an import.
struct COHERENCE_API TraceImportReport {
  std::size_t lines_read = 0;
  std::size_t comments = 0;
  std::size_t resources_registered = 0;
  std::size_t regions_registered = 0;
  std::size_t observations_published = 0;
  std::size_t counters_published = 0;
  std::size_t descriptions_skipped = 0;
  std::size_t rejected = 0;
  std::vector<TraceImportDiagnostic> diagnostics;
};

/// Imports a trace file into p sink, registering structure through
/// p registrar when one is available.  With p strict, the first malformed
/// line aborts the import and the error names the line.
COHERENCE_API Result<TraceImportReport> import_trace_file(
    const std::filesystem::path& path, IngestionSink& sink, StructureRegistrar* registrar,
    bool strict);

/// Imports trace text already in memory (used by tests and by embedded hosts).
COHERENCE_API Result<TraceImportReport> import_trace_text(
    std::string_view text, IngestionSink& sink, StructureRegistrar* registrar, bool strict);

/// Renders the canonical trace format for a set of observations.
COHERENCE_API std::string render_trace(const std::vector<Observation>& observations);

/// Imported-trace collector configuration.
struct COHERENCE_API TraceImportConfig {
  std::filesystem::path path;
  bool strict = true;
  std::string publisher_id = "pub.trace.import";
  std::string publisher_name = "trace-import";
};

/// Collector wrapper that imports one trace file during start().
class COHERENCE_API ImportedTraceBackend : public Collector {
 public:
  explicit ImportedTraceBackend(TraceImportConfig config = {});
  ~ImportedTraceBackend() override;

  ImportedTraceBackend(const ImportedTraceBackend&) = delete;
  ImportedTraceBackend& operator=(const ImportedTraceBackend&) = delete;

  std::string_view name() const noexcept override;
  std::vector<Capability> capabilities() const override;
  Status start(const CollectorContext& context) override;
  Status poll(const CollectorContext& context) override;
  Status stop() override;

  const TraceImportReport& report() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sol::coherence

#endif  // COHERENCE_BACKENDS_IMPORTED_TRACE_HPP
