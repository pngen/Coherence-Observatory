// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// cohobs-publisher -- an independent publisher process.
//
// Used by the multiprocess proofs and by operators who want a real remote
// publisher without writing code.  It connects over TCP, registers under its
// own publisher and boot identity, publishes a deterministic scenario and then
// optionally stays alive so that a supervisor can kill it.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "coherence/backends/synthetic.hpp"
#include "coherence/backend.hpp"
#include "coherence/client.hpp"
#include "coherence/version.hpp"

namespace {

/// Opens a file for writing without MSVC deprecation warnings.
std::FILE* open_for_write(const std::string& path) {
  std::FILE* file = nullptr;
#if defined(_WIN32)
  if (fopen_s(&file, path.c_str(), "wb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.c_str(), "wb");
#endif
  return file;
}

void print_usage() {
  std::cout <<
      "cohobs-publisher " << sol::coherence::library_version_string() << " -- remote publisher\n"
      "\n"
      "usage: cohobs-publisher --endpoint host:port --publisher <id> [options]\n"
      "  --scenario <name>   synthetic preset (default PING_PONG)\n"
      "  --count <n>         observations to publish (default 64)\n"
      "  --boot <value>      explicit boot identity (default: fresh)\n"
      "  --boot-file <file>  write the boot identity to a file after connecting\n"
      "  --ready-file <file> write a ready marker once registered\n"
      "  --hold <seconds>    stay alive after publishing, then exit\n"
      "  --publish-counters  also publish counter samples\n"
      "  --seed <value>      deterministic seed\n"
      "  --verbose           print per-observation outcomes\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string endpoint;
  std::string publisher = "pub.remote.1";
  std::string scenario = "PING_PONG";
  std::uint64_t count = 64;
  std::uint64_t boot = 0;
  std::string boot_file;
  std::string ready_file;
  double hold_seconds = 0.0;
  std::uint64_t seed = 0x5EEDC0FFEEull;
  bool publish_counters = false;
  bool verbose = false;

  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    auto next = [&i, argc, argv]() -> std::string {
      return i + 1 < argc ? std::string(argv[++i]) : std::string();
    };
    if (argument == "--help" || argument == "-h") {
      print_usage();
      return 0;
    } else if (argument == "--version") {
      std::cout << sol::coherence::library_version_string() << "\n";
      return 0;
    } else if (argument == "--endpoint") {
      endpoint = next();
    } else if (argument == "--publisher") {
      publisher = next();
    } else if (argument == "--scenario") {
      scenario = next();
    } else if (argument == "--count") {
      count = std::strtoull(next().c_str(), nullptr, 10);
    } else if (argument == "--boot") {
      boot = std::strtoull(next().c_str(), nullptr, 0);
    } else if (argument == "--boot-file") {
      boot_file = next();
    } else if (argument == "--ready-file") {
      ready_file = next();
    } else if (argument == "--hold") {
      hold_seconds = std::strtod(next().c_str(), nullptr);
    } else if (argument == "--seed") {
      seed = std::strtoull(next().c_str(), nullptr, 0);
    } else if (argument == "--publish-counters") {
      publish_counters = true;
    } else if (argument == "--verbose") {
      verbose = true;
    } else {
      std::cerr << "error: unknown option '" << argument << "'\n";
      print_usage();
      return 2;
    }
  }
  if (endpoint.empty()) {
    std::cerr << "error: --endpoint is required\n";
    print_usage();
    return 2;
  }

  std::string host;
  std::uint16_t port = 0;
  {
    const std::size_t colon = endpoint.rfind(':');
    if (colon == std::string::npos) {
      std::cerr << "error: endpoint must be host:port\n";
      return 2;
    }
    host = endpoint.substr(0, colon);
    port = static_cast<std::uint16_t>(std::strtoul(endpoint.substr(colon + 1).c_str(),
                                                   nullptr, 10));
  }

  const sol::coherence::Result<sol::coherence::SyntheticPreset> preset =
      sol::coherence::parse_synthetic_preset(scenario);
  if (!preset.ok()) {
    std::cerr << "error: " << preset.describe() << "\n";
    return 2;
  }
  sol::coherence::SyntheticConfig config = sol::coherence::make_preset_config(preset.value(), seed);
  config.publisher_id = publisher;
  config.publisher_name = publisher;
  config.publish_counters = publish_counters;

  sol::coherence::ClientOptions client_options;
  client_options.host = host;
  client_options.port = port;
  client_options.client_name = publisher;

  sol::coherence::PublisherRegistration registration;
  registration.id = sol::coherence::PublisherId{publisher};
  registration.boot = boot == 0 ? sol::coherence::make_publisher_boot_id()
                                : sol::coherence::PublisherBootId{boot};
  registration.observer = sol::coherence::ObserverId{"observer.remote"};
  registration.node = sol::coherence::NodeId{"node.remote"};
  registration.display_name = publisher;
  registration.provenance = sol::coherence::Provenance::SyntheticBackend;

  sol::coherence::ObservationPublisher remote_publisher;
  const sol::coherence::Status connected = remote_publisher.connect(client_options, registration);
  if (!connected.ok()) {
    std::cerr << "error: " << connected.describe() << "\n";
    return 3;
  }
  std::cout << "registered " << publisher << " boot " << remote_publisher.publisher_boot().value()
            << " epoch " << remote_publisher.coordinator_epoch().value() << std::endl;
  if (!boot_file.empty()) {
    if (std::FILE* file = open_for_write(boot_file)) {
      const std::string text =
          std::to_string(remote_publisher.publisher_boot().value()) + "\n";
      std::fwrite(text.data(), 1, text.size(), file);
      std::fclose(file);
    }
  }
  if (!ready_file.empty()) {
    if (std::FILE* file = open_for_write(ready_file)) {
      std::fwrite("ready\n", 1, 6, file);
      std::fclose(file);
    }
  }

  // Structure taken from the preset configuration; a remote publisher cannot
  // register structure, so only the observations are published.
  std::vector<sol::coherence::SyntheticStep> script;
  for (sol::coherence::SyntheticPreset entry : config.presets) {
    const std::vector<sol::coherence::SyntheticStep> preset_script =
        sol::coherence::make_preset_script(entry);
    script.insert(script.end(), preset_script.begin(), preset_script.end());
  }
  if (script.empty()) {
    std::cerr << "error: empty script\n";
    return 2;
  }

  sol::coherence::Nanos clock = sol::coherence::monotonic_now_ns();
  std::uint64_t published = 0;
  std::uint64_t rejected = 0;
  for (std::uint64_t index = 0; index < count; ++index) {
    const sol::coherence::SyntheticStep& step = script[index % script.size()];
    sol::coherence::Observation observation;
    observation.type = step.type;
    observation.timestamp_ns = clock;
    clock += step.spacing_ns;
    observation.direction = step.direction;
    observation.state_before = step.state_before;
    observation.state_after = step.state_after;
    observation.bytes = step.bytes;
    observation.lines = step.lines;
    observation.pages = step.pages;
    observation.locality = step.locality;
    observation.locality_declared = step.locality_declared;
    observation.precision = step.precision;
    observation.granularity = step.granularity;
    observation.provenance = sol::coherence::Provenance::SyntheticBackend;
    observation.topology_generation = remote_publisher.topology_generation();
    if (!step.region.empty()) {
      observation.region = sol::coherence::MemoryRegionId{step.region};
      observation.region_generation = sol::coherence::MemoryRegionGeneration{1};
    }
    if (!step.source.empty()) {
      sol::coherence::ResourceRef source;
      source.kind = step.source.rfind("cpu.", 0) == 0 ? sol::coherence::ResourceKind::Processor
                                                      : sol::coherence::ResourceKind::Accelerator;
      source.id = sol::coherence::ResourceId{step.source};
      source.generation = 1;
      observation.source = source;
    }
    if (!step.target.empty()) {
      sol::coherence::ResourceRef target;
      target.kind = sol::coherence::ResourceKind::MemoryDomain;
      target.id = sol::coherence::ResourceId{step.target};
      target.generation = 1;
      observation.target = target;
    }
    const sol::coherence::Result<sol::coherence::IngestionOutcome> outcome =
        remote_publisher.publish(std::move(observation));
    if (!outcome.ok() ||
        outcome.value().disposition == sol::coherence::IngestionDisposition::Rejected) {
      ++rejected;
      if (verbose) {
        std::cout << "rejected: " << outcome.describe() << std::endl;
      }
    } else {
      ++published;
    }
  }
  std::cout << "published " << published << " rejected " << rejected << std::endl;

  if (hold_seconds > 0.0) {
    const auto begin = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count() <
           hold_seconds) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      const sol::coherence::Status beat = remote_publisher.heartbeat();
      if (!beat.ok()) {
        std::cerr << "heartbeat failed: " << beat.describe() << "\n";
        break;
      }
    }
  }
  remote_publisher.close();
  return rejected == 0 ? 0 : 1;
}