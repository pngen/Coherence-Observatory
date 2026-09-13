// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// cohobs-admin -- administrative mutation.
//
// This is deliberately a separate executable from the read-only inspection
// tool: inspection must never be able to mutate coordinator state.

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "cli_support.hpp"
#include "coherence/version.hpp"

namespace {

void print_usage() {
  std::cout <<
      "cohobs-admin " << sol::coherence::library_version_string()
      << " -- administrative mutation (live coordinator only)\n"
      "\n"
      "usage: cohobs-admin --endpoint host:port <command> [arguments]\n"
      "\n"
      "commands:\n"
      "  fence <publisher> <reason>       fence every boot of a publisher identity\n"
      "  retire-region <region> <gen>     retire a region at an exact generation\n"
      "  bump-topology <reason>           advance the topology generation\n"
      "  save                             write durable state to the configured path\n"
      "\n"
      "reasons: administrative | connection-closed | liveness-deadline |\n"
      "         coordinator-restart | superseded-by-new-boot\n";
}

sol::coherence::Result<sol::coherence::FenceReason> parse_fence_reason(const std::string& text) {
  if (text == "administrative") {
    return sol::coherence::Result<sol::coherence::FenceReason>(sol::coherence::FenceReason::Administrative);
  }
  if (text == "connection-closed") {
    return sol::coherence::Result<sol::coherence::FenceReason>(
        sol::coherence::FenceReason::ConnectionClosed);
  }
  if (text == "liveness-deadline") {
    return sol::coherence::Result<sol::coherence::FenceReason>(sol::coherence::FenceReason::LivenessDeadline);
  }
  if (text == "coordinator-restart") {
    return sol::coherence::Result<sol::coherence::FenceReason>(sol::coherence::FenceReason::CoordinatorRestart);
  }
  if (text == "superseded-by-new-boot") {
    return sol::coherence::Result<sol::coherence::FenceReason>(
        sol::coherence::FenceReason::SupersededByNewBoot);
  }
  return sol::coherence::fail_as<sol::coherence::FenceReason>(sol::coherence::ErrorCode::InvalidArgument,
                                                    "unknown fence reason", text);
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

  coherence_cli::AdminReader reader;
  const sol::coherence::Status opened = reader.open(source);
  if (!opened.ok()) {
    std::cerr << "error: " << opened.describe() << "\n";
    return 2;
  }
  sol::coherence::ObservatoryAdminClient& client = reader.client();
  const std::string command = positional[0];

  if (command == "fence") {
    if (positional.size() < 2) {
      std::cerr << "error: fence requires a publisher identity\n";
      return 2;
    }
    sol::coherence::FenceReason reason = sol::coherence::FenceReason::Administrative;
    if (positional.size() >= 3) {
      const sol::coherence::Result<sol::coherence::FenceReason> parsed_reason =
          parse_fence_reason(positional[2]);
      if (!parsed_reason.ok()) {
        std::cerr << "error: " << parsed_reason.describe() << "\n";
        return 2;
      }
      reason = parsed_reason.value();
    }
    const sol::coherence::Result<sol::coherence::PublisherId> publisher =
        sol::coherence::PublisherId::parse(positional[1]);
    if (!publisher.ok()) {
      std::cerr << "error: " << publisher.describe() << "\n";
      return 2;
    }
    const sol::coherence::Status fenced =
        client.fence_publisher(publisher.value(), reason, "administrative fence");
    if (!fenced.ok()) {
      std::cerr << "error: " << fenced.describe() << "\n";
      return 3;
    }
    std::cout << "fenced " << positional[1] << "\n";
    return 0;
  }

  if (command == "retire-region") {
    if (positional.size() < 3) {
      std::cerr << "error: retire-region requires a region identity and a generation\n";
      return 2;
    }
    const sol::coherence::Result<sol::coherence::MemoryRegionId> region =
        sol::coherence::MemoryRegionId::parse(positional[1]);
    if (!region.ok()) {
      std::cerr << "error: " << region.describe() << "\n";
      return 2;
    }
    std::uint64_t generation = 0;
    for (char c : positional[2]) {
      if (c < '0' || c > '9') {
        std::cerr << "error: generation must be numeric\n";
        return 2;
      }
      generation = generation * 10u + static_cast<std::uint64_t>(c - '0');
    }
    const sol::coherence::Status retired = client.retire_region(
        region.value(), sol::coherence::MemoryRegionGeneration{generation},
        "administrative retirement");
    if (!retired.ok()) {
      std::cerr << "error: " << retired.describe() << "\n";
      return 3;
    }
    std::cout << "retired " << positional[1] << "\n";
    return 0;
  }

  if (command == "bump-topology") {
    const std::string reason = positional.size() > 1 ? positional[1] : "administrative";
    const sol::coherence::Result<sol::coherence::TopologyGeneration> generation =
        client.bump_topology(reason);
    if (!generation.ok()) {
      std::cerr << "error: " << generation.describe() << "\n";
      return 3;
    }
    std::cout << "topology generation advanced\n";
    return 0;
  }

  if (command == "save") {
    const sol::coherence::Result<sol::coherence::PersistenceReport> saved = client.save_state();
    if (!saved.ok()) {
      std::cerr << "error: " << saved.describe() << "\n";
      return 3;
    }
    std::cout << "state saved\n";
    return 0;
  }

  std::cerr << "error: unknown command '" << command << "'\n";
  print_usage();
  return 2;
}