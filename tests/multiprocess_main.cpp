// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "process_utils.hpp"
#include "test_framework.hpp"

int main(int argc, char** argv) {
  cotest::set_program_path(argc > 0 ? argv[0] : "");
  return cotest::run_all("coherence_observatory_multiprocess");
}
