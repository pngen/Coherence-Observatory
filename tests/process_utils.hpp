// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Process orchestration for the multiprocess proofs.  These helpers launch and
// hard-kill real operating-system processes; a hard kill is the only way the
// process-death scenarios are exercised.

#ifndef COHERENCE_TESTS_PROCESS_UTILS_HPP
#define COHERENCE_TESTS_PROCESS_UTILS_HPP

#include <chrono>
#include <cstdio>
#include <filesystem>
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
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace cotest {

namespace fs = std::filesystem;

/// A running or completed child process.
struct ChildProcess {
#if defined(_WIN32)
  void* process = nullptr;
  unsigned long identifier = 0;
#else
  int identifier = -1;
#endif
  bool running = false;
  int exit_code = -1;
  bool were_running = false;
};

/// Path of the currently running test executable, set from main().
inline std::string& program_path() {
  static std::string path;
  return path;
}

inline void set_program_path(const char* argv0) {
  program_path() = argv0 == nullptr ? std::string() : std::string(argv0);
}

/// Directory containing the currently running test executable.  Sibling
/// executables built by the same CMake project live here.
inline fs::path executable_directory() {
  std::error_code ignored;
  fs::path path = fs::absolute(fs::path(program_path()), ignored);
  return path.parent_path();
}

inline std::string quote_argument(const std::string& argument) {
  std::string out = "\"";
  for (char c : argument) {
    if (c == '"') {
      out.append("\\\"");
    } else {
      out.push_back(c);
    }
  }
  out.push_back('"');
  return out;
}

inline ChildProcess spawn_process(const fs::path& executable,
                                  const std::vector<std::string>& arguments,
                                  const fs::path& working_directory) {
  ChildProcess child;
  std::string command = quote_argument(executable.string());
  for (const std::string& argument : arguments) {
    command.push_back(' ');
    command.append(quote_argument(argument));
  }
#if defined(_WIN32)
  std::string mutable_command = command;
  STARTUPINFOA startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION information{};
  const std::string directory = working_directory.string();
  if (CreateProcessA(nullptr, mutable_command.data(), nullptr, nullptr, FALSE,
                     CREATE_NO_WINDOW, nullptr,
                     directory.empty() ? nullptr : directory.c_str(), &startup,
                     &information) == 0) {
    return child;
  }
  CloseHandle(information.hThread);
  child.process = information.hProcess;
  child.identifier = information.dwProcessId;
  child.running = true;
  child.were_running = true;
#else
  const pid_t pid = ::fork();
  if (pid == 0) {
    if (!working_directory.empty()) {
      (void)::chdir(working_directory.string().c_str());
    }
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(executable.string().c_str()));
    for (const std::string& argument : arguments) {
      argv.push_back(const_cast<char*>(argument.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(executable.string().c_str(), argv.data());
    ::_exit(127);
  }
  child.identifier = pid;
  child.running = pid > 0;
  child.were_running = child.running;
#endif
  return child;
}

/// Terminates the process immediately.  This is a hard kill, not a request.
inline bool kill_process(ChildProcess& child) {
  if (!child.running) {
    return false;
  }
#if defined(_WIN32)
  const BOOL killed = TerminateProcess(static_cast<HANDLE>(child.process), 137);
  if (killed == 0) {
    return false;
  }
  WaitForSingleObject(static_cast<HANDLE>(child.process), INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(child.process), &code);
  child.exit_code = static_cast<int>(code);
  CloseHandle(static_cast<HANDLE>(child.process));
  child.process = nullptr;
#else
  if (::kill(child.identifier, SIGKILL) != 0) {
    return false;
  }
  int status = 0;
  ::waitpid(child.identifier, &status, 0);
  child.exit_code = status;
#endif
  child.running = false;
  return true;
}

/// Waits for the process to exit on its own.
inline bool wait_process(ChildProcess& child, unsigned long milliseconds) {
  if (!child.running) {
    return true;
  }
#if defined(_WIN32)
  const DWORD result =
      WaitForSingleObject(static_cast<HANDLE>(child.process), milliseconds);
  if (result != WAIT_OBJECT_0) {
    return false;
  }
  DWORD code = 0;
  GetExitCodeProcess(static_cast<HANDLE>(child.process), &code);
  child.exit_code = static_cast<int>(code);
  CloseHandle(static_cast<HANDLE>(child.process));
  child.process = nullptr;
#else
  (void)milliseconds;
  int status = 0;
  if (::waitpid(child.identifier, &status, WNOHANG) != child.identifier) {
    return false;
  }
  child.exit_code = status;
#endif
  child.running = false;
  return true;
}

/// Ensures the child is gone, hard-killing it if it is still alive.
inline void ensure_terminated(ChildProcess& child) {
  if (child.running) {
    kill_process(child);
  }
}

/// Waits until \p path exists, polling the filesystem.
///
/// This is process-startup readiness only: it never bounds test logic, and
/// expiry is reported as a hard failure by the caller.
inline bool wait_for_file(const fs::path& path, unsigned long attempts = 600,
                          unsigned long sleep_milliseconds = 100) {
  std::error_code ignored;
  for (unsigned long attempt = 0; attempt < attempts; ++attempt) {
    if (fs::exists(path, ignored)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_milliseconds));
  }
  return false;
}

inline std::string read_text_file(const fs::path& path) {
  std::FILE* file = nullptr;
#if defined(_WIN32)
  if (fopen_s(&file, path.string().c_str(), "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.string().c_str(), "rb");
#endif
  if (file == nullptr) {
    return std::string();
  }
  std::string content;
  char buffer[256];
  std::size_t read = 0;
  while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
    content.append(buffer, read);
  }
  std::fclose(file);
  while (!content.empty() && (content.back() == '\n' || content.back() == '\r')) {
    content.pop_back();
  }
  return content;
}

/// A cleanup guard that hard-kills a child when it leaves scope.
class ProcessGuard {
 public:
  explicit ProcessGuard(ChildProcess& child) : child_(&child) {}
  ~ProcessGuard() { ensure_terminated(*child_); }
  ProcessGuard(const ProcessGuard&) = delete;
  ProcessGuard& operator=(const ProcessGuard&) = delete;

 private:
  ChildProcess* child_;
};

}  // namespace cotest

#endif  // COHERENCE_TESTS_PROCESS_UTILS_HPP
