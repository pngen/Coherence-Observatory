// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Durable state is versioned and integrity-checked, a corrupt file is rejected
// completely without partially applying, and recovery never promotes
// prior-run activity to current evidence.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

#include "example_support.hpp"

int main() {
  const std::filesystem::path state_path =
      std::filesystem::temp_directory_path() / "cohobs-example-persistence.bin";
  std::error_code ignored;
  std::filesystem::remove(state_path, ignored);

  sol::coherence::Observatory observatory(example::default_options());
  example::register_topology(observatory);
  const sol::coherence::Result<sol::coherence::PersistenceReport> saved =
      observatory.save_state(state_path);
  if (!saved.ok()) {
    std::cerr << "error: " << saved.describe() << "\n";
    return 1;
  }
  std::cout << "saved " << saved.value().bytes_written << " bytes in "
            << saved.value().sections << " sections\n";

  const std::uint64_t clean_fingerprint = observatory.state_fingerprint();

  // Corrupt one byte inside the payload and prove the load is refused and the
  // live state is untouched.
  {
    std::fstream file(state_path, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekp(size / 2);
    const char corrupt = static_cast<char>(0xA5);
    file.write(&corrupt, 1);
  }
  const sol::coherence::Result<sol::coherence::PersistenceReport> corrupted =
      observatory.load_state(state_path);
  std::cout << "corrupt load: " << (corrupted.ok() ? "accepted" : "rejected")
            << " (" << sol::coherence::to_string(corrupted.code()) << ")\n";
  const bool untouched = observatory.state_fingerprint() == clean_fingerprint;
  std::cout << "state untouched after failed load: " << example::bool_text(untouched) << "\n";

  std::filesystem::remove(state_path, ignored);
  return (!corrupted.ok() && untouched) ? 0 : 1;
}