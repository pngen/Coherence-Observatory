// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/version.hpp"

#include "coherence/protocol.hpp"

namespace sol::coherence {

namespace {
constexpr std::string_view kVersionString = "1.0.0";

#if defined(_MSC_VER)
constexpr std::string_view kCompilerTag = "msvc";
#elif defined(__clang__)
constexpr std::string_view kCompilerTag = "clang";
#elif defined(__GNUC__)
constexpr std::string_view kCompilerTag = "gcc";
#else
constexpr std::string_view kCompilerTag = "unknown";
#endif

#if defined(NDEBUG)
constexpr std::string_view kConfigTag = "release";
#else
constexpr std::string_view kConfigTag = "debug";
#endif

constexpr std::string_view kBuildIdentity =
#if defined(_MSC_VER)
    "1.0.0+msvc+"
#elif defined(__clang__)
    "1.0.0+clang+"
#elif defined(__GNUC__)
    "1.0.0+gcc+"
#else
    "1.0.0+unknown+"
#endif
#if defined(NDEBUG)
    "release"
#else
    "debug"
#endif
    ;
}  // namespace

Version library_version() noexcept { return Version{}; }

std::string_view library_version_string() noexcept { return kVersionString; }

std::uint16_t protocol_version() noexcept { return kWireProtocolVersion; }

std::string_view build_identity() noexcept { return kBuildIdentity; }

}  // namespace sol::coherence
