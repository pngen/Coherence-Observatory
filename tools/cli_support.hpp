// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Shared command-line support.  Not installed.

#ifndef COHERENCE_TOOLS_CLI_SUPPORT_HPP
#define COHERENCE_TOOLS_CLI_SUPPORT_HPP

#include <memory>
#include <string>
#include <vector>

#include "coherence/client.hpp"
#include "coherence/observatory.hpp"
#include "coherence/snapshot.hpp"

namespace coherence_cli {

/// Where a command reads its evidence from.
struct Source {
  std::string endpoint;
  std::string state_path;
  bool from_state = false;
};

/// Parses "--endpoint host:port" or "--state <file>" from \p arguments.
sol::coherence::Status parse_source(const std::vector<std::string>& arguments, Source* source,
                               std::vector<std::string>* positional);

/// Splits an endpoint into host and port.
sol::coherence::Status split_endpoint(const std::string& endpoint, std::string* host,
                                 std::uint16_t* port);

/// A read-only view over either a live coordinator or a durable state file.
class Reader {
 public:
  Reader();
  ~Reader();

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  sol::coherence::Status open(const Source& source);
  bool open() const noexcept;

  sol::coherence::Result<sol::coherence::SnapshotPtr> snapshot(
      const sol::coherence::FindingsOptions& options = {}) const;
  sol::coherence::Result<std::vector<sol::coherence::Finding>> findings(
      const sol::coherence::FindingsOptions& options = {}) const;
  sol::coherence::Result<sol::coherence::AttributionResult> attribution(
      const sol::coherence::MemoryRegionId& region) const;

  /// Non-null only when the reader opened a durable state file.  Analysis that
  /// requires full local state (per-pair tables) is available then.
  sol::coherence::Observatory* local_observatory() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  friend class AdminReader;
};

/// A mutating view over a live coordinator.
class AdminReader {
 public:
  AdminReader();
  ~AdminReader();

  AdminReader(const AdminReader&) = delete;
  AdminReader& operator=(const AdminReader&) = delete;

  sol::coherence::Status open(const Source& source);
  bool open() const noexcept;
  sol::coherence::ObservatoryAdminClient& client();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ---- Rendering ---------------------------------------------------------

/// Renders an aligned table with a header row.
std::string render_table(const std::vector<std::string>& headers,
                         const std::vector<std::vector<std::string>>& rows);

std::string bool_text(bool value);
std::string bytes_text(std::uint64_t value);
std::string rate_text(double value);
std::string nanos_text(std::int64_t value);
std::string precision_breakdown(const sol::coherence::AggregateValue& value);
std::string reality_breakdown(const sol::coherence::AggregateValue& value);

/// Renders every finding of a snapshot as a table row plus its explanation.
std::string render_findings(const std::vector<sol::coherence::Finding>& findings, bool explain);

/// Deterministic JSON rendering of a whole snapshot.
std::string render_snapshot_json(const sol::coherence::Snapshot& snapshot);

/// Prints the standard capability table.
std::string render_capabilities(const std::vector<sol::coherence::Capability>& capabilities);

}  // namespace coherence_cli

#endif  // COHERENCE_TOOLS_CLI_SUPPORT_HPP