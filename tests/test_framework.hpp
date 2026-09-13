// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Minimal deterministic test framework.  No third-party dependency, no
// timeouts, no hidden global state beyond the registry.

#ifndef COHERENCE_TESTS_TEST_FRAMEWORK_HPP
#define COHERENCE_TESTS_TEST_FRAMEWORK_HPP

#include <cstdio>
#include <string>
#include <vector>

namespace cotest {

struct TestCase {
  const char* name = "";
  const char* file = "";
  int line = 0;
  void (*body)() = nullptr;
};

inline std::vector<TestCase>& registry() {
  static std::vector<TestCase> tests;
  return tests;
}

inline std::vector<std::string>& failures() {
  static std::vector<std::string> recorded;
  return recorded;
}

inline std::uint64_t& check_count() {
  static std::uint64_t count = 0;
  return count;
}

struct Registrar {
  Registrar(const char* name, const char* file, int line, void (*body)()) {
    registry().push_back(TestCase{name, file, line, body});
  }
};

inline void record_failure(const char* file, int line, const std::string& message) {
  std::string text(file);
  text.append(":");
  text.append(std::to_string(line));
  text.append(": ");
  text.append(message);
  failures().push_back(std::move(text));
  std::printf("  FAIL %s\n", failures().back().c_str());
}

inline int run_all(const char* suite) {
  std::printf("=== %s: %llu tests ===\n", suite,
              static_cast<unsigned long long>(registry().size()));
  std::size_t passed = 0;
  for (const TestCase& test : registry()) {
    const std::size_t before = failures().size();
    std::printf("[ RUN  ] %s\n", test.name);
    test.body();
    if (failures().size() == before) {
      ++passed;
      std::printf("[  OK  ] %s\n", test.name);
    } else {
      std::printf("[ FAIL ] %s (%llu failures)\n", test.name,
                  static_cast<unsigned long long>(failures().size() - before));
    }
  }
  std::printf("=== %s: %llu/%llu passed, %llu checks, %llu failures ===\n", suite,
              static_cast<unsigned long long>(passed),
              static_cast<unsigned long long>(registry().size()),
              static_cast<unsigned long long>(check_count()),
              static_cast<unsigned long long>(failures().size()));
  return failures().empty() ? 0 : 1;
}

}  // namespace cotest

#define CO_TEST(name)                                                        \
  static void name();                                                        \
  static ::cotest::Registrar cotest_registrar_##name(#name, __FILE__, __LINE__, &name); \
  static void name()

#define CO_CHECK(condition)                                                  \
  do {                                                                       \
    ++::cotest::check_count();                                               \
    if (!(condition)) {                                                      \
      ::cotest::record_failure(__FILE__, __LINE__, "CO_CHECK failed: " #condition); \
    }                                                                        \
  } while (false)

#define CO_CHECK_EQ(actual, expected)                                        \
  do {                                                                       \
    ++::cotest::check_count();                                               \
    const auto cotest_actual = (actual);                                     \
    const auto cotest_expected = (expected);                                 \
    if (!(cotest_actual == cotest_expected)) {                               \
      ::cotest::record_failure(__FILE__, __LINE__,                           \
                               std::string("CO_CHECK_EQ failed: " #actual)); \
    }                                                                        \
  } while (false)

#define CO_REQUIRE(condition)                                                \
  do {                                                                       \
    ++::cotest::check_count();                                               \
    if (!(condition)) {                                                      \
      ::cotest::record_failure(__FILE__, __LINE__,                           \
                               "CO_REQUIRE failed: " #condition);            \
      return;                                                                \
    }                                                                        \
  } while (false)

#endif  // COHERENCE_TESTS_TEST_FRAMEWORK_HPP
