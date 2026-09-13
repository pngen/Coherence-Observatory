# Coherence Observatory 1.0.0
# Copyright 2026 Summon Software Labs.
# Apache License 2.0 -- see LICENSE.
#
# Vendor-neutral C++20 runtime for observing, attributing, correlating and
# preserving evidence about coherence behaviour across heterogeneous memory
# infrastructure.  No third-party dependencies.

cmake_minimum_required(VERSION 3.20)

project(CoherenceObservatory
        VERSION 1.0.0
        DESCRIPTION "Coherence observation, attribution and evidence runtime"
        HOMEPAGE_URL "https://github.com/pngen/Coherence-Observatory"
        LANGUAGES CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(COHERENCE_OBSERVATORY_BUILD_TESTS "Build the Coherence Observatory test suites" ON)
option(COHERENCE_OBSERVATORY_BUILD_EXAMPLES "Build the Coherence Observatory examples" ON)
option(COHERENCE_OBSERVATORY_BUILD_TOOLS "Build the Coherence Observatory command line tools" ON)
option(COHERENCE_OBSERVATORY_BUILD_BENCHMARKS "Build the Coherence Observatory benchmarks" ON)
option(COHERENCE_OBSERVATORY_WARNINGS_AS_ERRORS "Treat first-party warnings as errors" ON)
set(COHERENCE_OBSERVATORY_SANITIZE "" CACHE STRING
    "Sanitizer to enable (empty, address, thread)")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT CMAKE_CXX_STANDARD)
  set(CMAKE_CXX_STANDARD 20)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

find_package(Threads REQUIRED)

add_library(coherence_observatory_warnings INTERFACE)
if(MSVC)
  target_compile_options(coherence_observatory_warnings INTERFACE
      /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus /Zc:preprocessor /MP)
  if(COHERENCE_OBSERVATORY_WARNINGS_AS_ERRORS)
    target_compile_options(coherence_observatory_warnings INTERFACE /WX)
  endif()
  target_compile_definitions(coherence_observatory_warnings INTERFACE
      WIN32_LEAN_AND_MEAN NOMINMAX)
else()
  target_compile_options(coherence_observatory_warnings INTERFACE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
  if(COHERENCE_OBSERVATORY_WARNINGS_AS_ERRORS)
    target_compile_options(coherence_observatory_warnings INTERFACE -Werror)
  endif()
endif()

set(COHERENCE_OBSERVATORY_SOURCES
    src/aggregate.cpp
    src/analysis.cpp
    src/attribution.cpp
    src/backend.cpp
    src/backends/cpu_os.cpp
    src/backends/imported_trace.cpp
    src/backends/nvidia.cpp
    src/backends/synthetic.cpp
    src/client.cpp
    src/coordinator.cpp
    src/cost.cpp
    src/error.cpp
    src/explanation.cpp
    src/ids.cpp
    src/locality.cpp
    src/observation.cpp
    src/observatory.cpp
    src/persistence.cpp
    src/platform.cpp
    src/precision.cpp
    src/protocol.cpp
    src/provenance.cpp
    src/publisher.cpp
    src/resource.cpp
    src/region.cpp
    src/state.cpp
    src/taxonomy.cpp
    src/text_util.cpp
    src/time.cpp
    src/topology.cpp
    src/transport.cpp
    src/version.cpp
    src/wire.cpp)

add_library(coherence_observatory STATIC ${COHERENCE_OBSERVATORY_SOURCES})
add_library(SummonSoftwareLabs::CoherenceObservatory ALIAS coherence_observatory)

set_target_properties(coherence_observatory PROPERTIES
    # The exported (installed) target keeps the documented public name.
    EXPORT_NAME CoherenceObservatory
    OUTPUT_NAME coherence_observatory
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

# Consumers inherit the C++20 requirement through the installed package.
target_compile_features(coherence_observatory PUBLIC cxx_std_20)

target_include_directories(coherence_observatory
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(coherence_observatory
    PUBLIC Threads::Threads
    PRIVATE $<BUILD_INTERFACE:coherence_observatory_warnings>)

if(WIN32)
  target_link_libraries(coherence_observatory PRIVATE ws2_32)
endif()

if(COHERENCE_OBSERVATORY_SANITIZE)
  if(MSVC)
    target_compile_options(coherence_observatory PUBLIC
        /fsanitize=${COHERENCE_OBSERVATORY_SANITIZE} /Zi)
  else()
    target_compile_options(coherence_observatory PUBLIC
        -fsanitize=${COHERENCE_OBSERVATORY_SANITIZE} -fno-omit-frame-pointer -g)
    target_link_options(coherence_observatory PUBLIC
        -fsanitize=${COHERENCE_OBSERVATORY_SANITIZE})
  endif()
endif()

install(DIRECTORY include/coherence
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        FILES_MATCHING PATTERN "*.hpp")

install(TARGETS coherence_observatory
        EXPORT CoherenceObservatoryTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(EXPORT CoherenceObservatoryTargets
        FILE CoherenceObservatoryTargets.cmake
        NAMESPACE SummonSoftwareLabs::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CoherenceObservatory)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CoherenceObservatoryConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/CoherenceObservatoryConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CoherenceObservatory)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/CoherenceObservatoryConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/CoherenceObservatoryConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/CoherenceObservatoryConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/CoherenceObservatory)

install(FILES LICENSE README.md
        DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/CoherenceObservatory
        OPTIONAL)

if(COHERENCE_OBSERVATORY_BUILD_TOOLS)
  add_executable(cohobs tools/cohobs.cpp tools/cli_support.cpp)
  target_link_libraries(cohobs PRIVATE SummonSoftwareLabs::CoherenceObservatory
                                      coherence_observatory_warnings)

  add_executable(cohobs-admin tools/cohobs_admin.cpp tools/cli_support.cpp)
  target_link_libraries(cohobs-admin PRIVATE SummonSoftwareLabs::CoherenceObservatory
                                              coherence_observatory_warnings)

  add_executable(cohobsd tools/cohobsd.cpp)
  target_link_libraries(cohobsd PRIVATE SummonSoftwareLabs::CoherenceObservatory
                                         coherence_observatory_warnings)

  add_executable(cohobs-publisher tools/cohobs_publisher.cpp)
  target_link_libraries(cohobs-publisher PRIVATE SummonSoftwareLabs::CoherenceObservatory
                                                 coherence_observatory_warnings)

  install(TARGETS cohobs cohobs-admin cohobsd cohobs-publisher
          RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(COHERENCE_OBSERVATORY_BUILD_EXAMPLES)
  set(COHERENCE_OBSERVATORY_EXAMPLES
      basic_exact_observation
      remote_access_attribution
      ping_pong_detection
      aggregate_counter_partial_attribution
      publisher_fencing
      coordinator_restart
      false_sharing_granularity
      persistence_recovery)
  foreach(example ${COHERENCE_OBSERVATORY_EXAMPLES})
    add_executable(example_${example} examples/${example}.cpp)
    target_link_libraries(example_${example}
        PRIVATE SummonSoftwareLabs::CoherenceObservatory coherence_observatory_warnings)
  endforeach()
endif()

if(COHERENCE_OBSERVATORY_BUILD_BENCHMARKS)
  add_executable(cohobs-bench benchmarks/bench_main.cpp)
  target_link_libraries(cohobs-bench PRIVATE SummonSoftwareLabs::CoherenceObservatory
                                                coherence_observatory_warnings)
endif()

if(COHERENCE_OBSERVATORY_BUILD_TESTS)
  enable_testing()

  add_executable(coherence_observatory_tests
      tests/test_main.cpp
      tests/test_ids_and_validation.cpp
      tests/test_ingestion_and_ordering.cpp
      tests/test_counters.cpp
      tests/test_generations_and_staleness.cpp
      tests/test_attribution.cpp
      tests/test_aggregation.cpp
      tests/test_analysis.cpp
      tests/test_hardening.cpp
      tests/test_explanations_and_snapshots.cpp
      tests/test_protocol_frames.cpp
      tests/test_persistence.cpp
      tests/test_property.cpp
      tests/test_race.cpp
      tests/test_backends.cpp)
  target_link_libraries(coherence_observatory_tests
      PRIVATE SummonSoftwareLabs::CoherenceObservatory coherence_observatory_warnings)
  target_include_directories(coherence_observatory_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME coherence_observatory_tests COMMAND coherence_observatory_tests)

  add_executable(coherence_observatory_multiprocess
      tests/multiprocess_main.cpp
      tests/test_multiprocess.cpp
      tests/test_protocol_adversarial.cpp)
  target_link_libraries(coherence_observatory_multiprocess
      PRIVATE SummonSoftwareLabs::CoherenceObservatory coherence_observatory_warnings)
  target_include_directories(coherence_observatory_multiprocess
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  add_test(NAME coherence_observatory_multiprocess COMMAND coherence_observatory_multiprocess)
endif()
