// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// cohobsd -- the Coherence Observatory coordinator daemon.
//
// Runs the authoritative observatory in its own operating-system process,
// serves real framed TCP transport, and optionally hosts in-process
// collectors.  It prints the bound endpoint on stdout so that supervisors and
// tests can discover an ephemeral port.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "coherence/backend.hpp"
#include "coherence/backends/cpu_os.hpp"
#include "coherence/backends/imported_trace.hpp"
#include "coherence/backends/nvidia.hpp"
#include "coherence/backends/synthetic.hpp"
#include "coherence/coordinator.hpp"
#include "coherence/version.hpp"

namespace {

#if defined(_WIN32)
std::atomic<bool> g_stop{false};

BOOL WINAPI console_handler(DWORD signal) {
  if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
    g_stop.store(true);
    return TRUE;
  }
  return FALSE;
}
#else
std::atomic<bool> g_stop{false};

void console_handler(int) { g_stop.store(true); }
#endif

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
      "cohobsd " << sol::coherence::library_version_string() << " -- coherence coordinator\n"
      "\n"
      "usage: cohobsd [options]\n"
      "  --host <address>        bind address (default 127.0.0.1)\n"
      "  --port <port>           listen port; 0 selects an ephemeral port\n"
      "  --state <file>          durable state file\n"
      "  --no-load               do not load an existing state file\n"
      "  --no-save               do not save on shutdown\n"
      "  --collect synthetic     host the synthetic collector in process\n"
      "  --collect cpu-os        host the real CPU/OS discovery collector\n"
      "  --collect nvidia        host the NVIDIA discovery collector\n"
      "  --cpu-workload          run the real CPU workload in the CPU/OS collector\n"
      "  --gpu-workload          run the real CUDA memory workload\n"
      "  --trace <file>          import a trace file at startup\n"
      "  --seed <value>          deterministic collector seed\n"
      "  --endpoint-file <file>  write the bound endpoint to a file once listening\n"
      "  --run-seconds <value>   exit after this many seconds (0 = run until stopped)\n";
}

}  // namespace

int main(int argc, char** argv) {
  sol::coherence::CoordinatorOptions options;
  std::vector<std::string> collectors;
  std::string trace_path;
  std::uint64_t seed = 0x5EEDC0FFEEull;
  std::string endpoint_file;
  double run_seconds = 0.0;
  bool cpu_workload = false;
  bool gpu_workload = false;

  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    auto next = [&i, argc, argv]() -> std::string {
      return i + 1 < argc ? std::string(argv[++i]) : std::string();
    };
    if (argument == "--help" || argument == "-h") {
      print_usage();
      return 0;
    } else if (argument == "--version") {
      std::cout << sol::coherence::library_version_string() << " " << sol::coherence::build_identity()
                << "\n";
      return 0;
    } else if (argument == "--host") {
      options.bind_host = next();
    } else if (argument == "--port") {
      options.port = static_cast<std::uint16_t>(std::strtoul(next().c_str(), nullptr, 10));
    } else if (argument == "--state") {
      options.state_path = next();
    } else if (argument == "--no-load") {
      options.load_on_start = false;
    } else if (argument == "--no-save") {
      options.save_on_shutdown = false;
    } else if (argument == "--collect") {
      collectors.push_back(next());
    } else if (argument == "--trace") {
      trace_path = next();
    } else if (argument == "--endpoint-file") {
      endpoint_file = next();
    } else if (argument == "--seed") {
      seed = std::strtoull(next().c_str(), nullptr, 0);
    } else if (argument == "--run-seconds") {
      run_seconds = std::strtod(next().c_str(), nullptr);
    } else if (argument == "--cpu-workload") {
      cpu_workload = true;
    } else if (argument == "--gpu-workload") {
      gpu_workload = true;
    } else {
      std::cerr << "error: unknown option '" << argument << "'\n";
      print_usage();
      return 2;
    }
  }

  sol::coherence::CoordinatorServer server(options);
  const sol::coherence::Status started = server.start();
  if (!started.ok()) {
    std::cerr << "error: " << started.describe() << "\n";
    return 3;
  }

  std::cout << "listening " << options.bind_host << ":" << server.port() << std::endl;
  if (!endpoint_file.empty()) {
    if (std::FILE* file = open_for_write(endpoint_file)) {
      const std::string text = options.bind_host + ":" + std::to_string(server.port()) + "\n";
      std::fwrite(text.data(), 1, text.size(), file);
      std::fclose(file);
    }
  }

  std::unique_ptr<sol::coherence::LocalPublisherHost> host;
  sol::coherence::CollectorRunner runner;
  if (!collectors.empty() || !trace_path.empty()) {
    sol::coherence::PublisherRegistration registration;
    registration.id = sol::coherence::PublisherId{"pub.host.local"};
    registration.boot = sol::coherence::make_publisher_boot_id();
    registration.observer = sol::coherence::ObserverId{"observer.local"};
    registration.node = sol::coherence::NodeId{"node.local"};
    registration.display_name = "in-process collector host";
    registration.provenance = collectors.empty() ? sol::coherence::Provenance::ImportedTrace
                                                 : sol::coherence::Provenance::SyntheticBackend;
    host = std::make_unique<sol::coherence::LocalPublisherHost>(server.observatory(), registration);
    const sol::coherence::Status host_started = host->start();
    if (!host_started.ok()) {
      std::cerr << "error: " << host_started.describe() << "\n";
      server.stop();
      return 3;
    }
    for (const std::string& name : collectors) {
      if (name == "synthetic") {
        sol::coherence::SyntheticConfig config =
            sol::coherence::make_preset_config(sol::coherence::SyntheticPreset::PingPong, seed);
        config.publish_counters = true;
        config.observation_budget = 512;
        runner.add(std::make_shared<sol::coherence::SyntheticBackend>(config));
      } else if (name == "cpu-os") {
        sol::coherence::CpuOsConfig config;
        config.run_workload = cpu_workload;
        runner.add(std::make_shared<sol::coherence::CpuOsBackend>(config));
      } else if (name == "nvidia") {
        sol::coherence::NvidiaConfig config;
        config.run_workload = gpu_workload;
        runner.add(std::make_shared<sol::coherence::NvidiaBackend>(config));
      } else {
        std::cerr << "error: unknown collector '" << name << "'\n";
        server.stop();
        return 2;
      }
    }
    if (!trace_path.empty()) {
      sol::coherence::TraceImportConfig config;
      config.path = trace_path;
      runner.add(std::make_shared<sol::coherence::ImportedTraceBackend>(config));
    }
    const sol::coherence::Status runner_started = runner.start(host.get(), host.get(), seed);
    if (!runner_started.ok()) {
      std::cerr << "error: " << runner_started.describe() << "\n";
      server.stop();
      return 3;
    }
  }

#if defined(_WIN32)
  SetConsoleCtrlHandler(console_handler, TRUE);
#else
  std::signal(SIGINT, console_handler);
  std::signal(SIGTERM, console_handler);
#endif

  const auto begin = std::chrono::steady_clock::now();
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (run_seconds > 0.0) {
      const double elapsed =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
      if (elapsed >= run_seconds) {
        break;
      }
    }
  }

  runner.stop();
  const sol::coherence::Status stopped = server.stop();
  std::cout << "stopped" << std::endl;
  if (!stopped.ok()) {
    std::cerr << "error: " << stopped.describe() << "\n";
    return 4;
  }
  return 0;
}