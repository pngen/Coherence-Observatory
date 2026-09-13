// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Deterministic error reporting.  The runtime does not throw for expected
// failure modes: every fallible operation returns a Status or Result<T>.

#ifndef COHERENCE_ERROR_HPP
#define COHERENCE_ERROR_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "coherence/export.hpp"

namespace sol::coherence {

/// Stable, exhaustive failure taxonomy.
///
/// Codes are grouped by the layer that produces them so that protocol,
/// persistence and ingestion failures remain distinguishable by callers.
enum class ErrorCode : std::uint16_t {
  Ok = 0,

  // Generic argument / capacity failures.
  InvalidArgument = 1,
  OutOfRange = 2,
  TooLarge = 3,
  TooMany = 4,
  NotFound = 5,
  AlreadyExists = 6,
  Conflict = 7,
  Capacity = 8,
  Overflow = 9,
  Saturated = 10,
  Busy = 11,
  Shutdown = 12,
  IoError = 13,
  Internal = 14,

  // Identity / authority failures.
  Unauthorized = 15,
  StaleEpoch = 16,
  StaleBoot = 17,
  StaleGeneration = 18,
  StaleSequence = 19,
  RetiredEntity = 20,
  UnsupportedCapability = 21,
  PrecisionInsufficient = 22,
  Duplicate = 23,

  // Protocol failures.
  InvalidMagic = 30,
  UnsupportedVersion = 31,
  UnknownMessageType = 32,
  InvalidFlags = 33,
  Truncated = 34,
  IntegrityFailure = 35,
  ProtocolError = 36,
  TrailingBytes = 37,
  ConnectionClosed = 38,

  // Persistence failures.
  CorruptState = 50,
  UnsupportedStateVersion = 51,
  EmptyState = 52,
};

/// Human-readable, stable rendering of an error code.
COHERENCE_API std::string_view to_string(ErrorCode code) noexcept;

/// Error code plus bounded human-readable context.
class COHERENCE_API Error {
 public:
  Error() = default;

  Error(ErrorCode code, std::string message)
      : code_(code), message_(std::move(message)) {}

  Error(ErrorCode code, std::string message, std::string context)
      : code_(code), message_(std::move(message)), context_(std::move(context)) {}

  ErrorCode code() const noexcept { return code_; }
  bool ok() const noexcept { return code_ == ErrorCode::Ok; }

  const std::string& message() const noexcept { return message_; }
  const std::string& context() const noexcept { return context_; }

  /// "code: message (context)" -- deterministic.
  std::string describe() const;

 private:
  ErrorCode code_ = ErrorCode::Ok;
  std::string message_;
  std::string context_;
};

/// Result of an operation that produces no value.
class COHERENCE_API Status {
 public:
  Status() = default;
  Status(Error error) : error_(std::move(error)) {}  // NOLINT: implicit by design

  bool ok() const noexcept { return error_.ok(); }
  explicit operator bool() const noexcept { return ok(); }

  const Error& error() const noexcept { return error_; }
  ErrorCode code() const noexcept { return error_.code(); }
  std::string describe() const { return error_.describe(); }

 private:
  Error error_;
};

/// Result of an operation that produces a value on success.
template <class T>
class Result {
 public:
  Result(T value) : storage_(std::move(value)) {}   // NOLINT: implicit by design
  Result(Error error) : storage_(std::move(error)) {}

  bool ok() const noexcept { return storage_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  const T& value() const { return std::get<0>(storage_); }
  T& value() { return std::get<0>(storage_); }
  T&& take() { return std::move(std::get<0>(storage_)); }

  const Error& error() const noexcept { return std::get<1>(storage_); }
  ErrorCode code() const noexcept { return ok() ? ErrorCode::Ok : error().code(); }
  std::string describe() const { return ok() ? std::string("ok") : error().describe(); }

  /// Value on success, p fallback otherwise.  For callers that can proceed
  /// with a conservative default.
  T value_or(T fallback) const {
    return ok() ? std::get<0>(storage_) : std::move(fallback);
  }

 private:
  std::variant<T, Error> storage_;
};

/// Convenience constructors.
inline Status ok_status() noexcept { return Status(); }

inline Status fail(ErrorCode code, std::string message) {
  return Status(Error(code, std::move(message)));
}

inline Status fail(ErrorCode code, std::string message, std::string context) {
  return Status(Error(code, std::move(message), std::move(context)));
}

template <class T>
Result<T> fail_as(ErrorCode code, std::string message) {
  return Result<T>(Error(code, std::move(message)));
}

template <class T>
Result<T> fail_as(ErrorCode code, std::string message, std::string context) {
  return Result<T>(Error(code, std::move(message), std::move(context)));
}

}  // namespace sol::coherence

#endif  // COHERENCE_ERROR_HPP
