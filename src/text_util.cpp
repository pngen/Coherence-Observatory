// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "text_util.hpp"

#include <array>
#include <cstdio>
#include <cstring>

namespace sol::coherence::detail {

bool iequals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'a' && ca <= 'z') {
      ca = static_cast<char>(ca - 'a' + 'A');
    }
    if (cb >= 'a' && cb <= 'z') {
      cb = static_cast<char>(cb - 'a' + 'A');
    }
    if (ca != cb) {
      return false;
    }
  }
  return true;
}

std::string to_upper(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    if (c >= 'a' && c <= 'z') {
      out.push_back(static_cast<char>(c - 'a' + 'A'));
    } else {
      out.push_back(c);
    }
  }
  return out;
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) {
      out.append(separator);
    }
    out.append(parts[i]);
  }
  return out;
}

std::string format_u64(std::uint64_t value) {
  std::array<char, 32> buffer{};
  const int written = std::snprintf(buffer.data(), buffer.size(), "%llu",
                                    static_cast<unsigned long long>(value));
  if (written <= 0) {
    return std::string("0");
  }
  return std::string(buffer.data(), static_cast<std::size_t>(written));
}

std::string format_i64(std::int64_t value) {
  std::array<char, 32> buffer{};
  const int written = std::snprintf(buffer.data(), buffer.size(), "%lld",
                                    static_cast<long long>(value));
  if (written <= 0) {
    return std::string("0");
  }
  return std::string(buffer.data(), static_cast<std::size_t>(written));
}

std::string format_double(double value, int decimals) {
  if (!(value == value)) {  // NaN
    return std::string("nan");
  }
  if (value > 1.0e300) {
    return std::string("inf");
  }
  if (value < -1.0e300) {
    return std::string("-inf");
  }
  if (decimals < 0) {
    decimals = 0;
  }
  if (decimals > 9) {
    decimals = 9;
  }
  std::array<char, 64> buffer{};
  const int written = std::snprintf(buffer.data(), buffer.size(), "%.*f", decimals, value);
  if (written <= 0) {
    return std::string("0");
  }
  return std::string(buffer.data(), static_cast<std::size_t>(written));
}

std::string format_bytes(std::uint64_t bytes) {
  static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
  double value = static_cast<double>(bytes);
  std::size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < (sizeof(units) / sizeof(units[0]))) {
    value /= 1024.0;
    ++unit;
  }
  if (unit == 0) {
    return format_u64(bytes) + " B";
  }
  return format_double(value, 2) + " " + units[unit];
}

std::string truncate(std::string_view text, std::size_t max_length) {
  if (text.size() <= max_length) {
    return std::string(text);
  }
  if (max_length <= 3) {
    return std::string(text.substr(0, max_length));
  }
  std::string out(text.substr(0, max_length - 3));
  out.append("...");
  return out;
}

std::uint64_t fnv1a_init() noexcept { return 1469598103934665603ull; }

std::uint64_t fnv1a(std::uint64_t state, const void* data, std::size_t size) noexcept {
  const auto* bytes = static_cast<const unsigned char*>(data);
  for (std::size_t i = 0; i < size; ++i) {
    state ^= static_cast<std::uint64_t>(bytes[i]);
    state *= 1099511628211ull;
  }
  return state;
}

std::uint64_t fnv1a_u64(std::uint64_t state, std::uint64_t value) noexcept {
  unsigned char buffer[8];
  for (int i = 0; i < 8; ++i) {
    buffer[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xFFu);
  }
  return fnv1a(state, buffer, sizeof(buffer));
}

std::uint64_t fnv1a_str(std::uint64_t state, std::string_view text) noexcept {
  state = fnv1a_u64(state, static_cast<std::uint64_t>(text.size()));
  return fnv1a(state, text.data(), text.size());
}

}  // namespace sol::coherence::detail
