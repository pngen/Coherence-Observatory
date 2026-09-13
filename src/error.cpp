// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/error.hpp"

namespace sol::coherence {

std::string_view to_string(ErrorCode code) noexcept {
  switch (code) {
    case ErrorCode::Ok: return "Ok";
    case ErrorCode::InvalidArgument: return "InvalidArgument";
    case ErrorCode::OutOfRange: return "OutOfRange";
    case ErrorCode::TooLarge: return "TooLarge";
    case ErrorCode::TooMany: return "TooMany";
    case ErrorCode::NotFound: return "NotFound";
    case ErrorCode::AlreadyExists: return "AlreadyExists";
    case ErrorCode::Conflict: return "Conflict";
    case ErrorCode::Capacity: return "Capacity";
    case ErrorCode::Overflow: return "Overflow";
    case ErrorCode::Saturated: return "Saturated";
    case ErrorCode::Busy: return "Busy";
    case ErrorCode::Shutdown: return "Shutdown";
    case ErrorCode::IoError: return "IoError";
    case ErrorCode::Internal: return "Internal";
    case ErrorCode::Unauthorized: return "Unauthorized";
    case ErrorCode::StaleEpoch: return "StaleEpoch";
    case ErrorCode::StaleBoot: return "StaleBoot";
    case ErrorCode::StaleGeneration: return "StaleGeneration";
    case ErrorCode::StaleSequence: return "StaleSequence";
    case ErrorCode::RetiredEntity: return "RetiredEntity";
    case ErrorCode::UnsupportedCapability: return "UnsupportedCapability";
    case ErrorCode::PrecisionInsufficient: return "PrecisionInsufficient";
    case ErrorCode::Duplicate: return "Duplicate";
    case ErrorCode::InvalidMagic: return "InvalidMagic";
    case ErrorCode::UnsupportedVersion: return "UnsupportedVersion";
    case ErrorCode::UnknownMessageType: return "UnknownMessageType";
    case ErrorCode::InvalidFlags: return "InvalidFlags";
    case ErrorCode::Truncated: return "Truncated";
    case ErrorCode::IntegrityFailure: return "IntegrityFailure";
    case ErrorCode::ProtocolError: return "ProtocolError";
    case ErrorCode::TrailingBytes: return "TrailingBytes";
    case ErrorCode::ConnectionClosed: return "ConnectionClosed";
    case ErrorCode::CorruptState: return "CorruptState";
    case ErrorCode::UnsupportedStateVersion: return "UnsupportedStateVersion";
    case ErrorCode::EmptyState: return "EmptyState";
  }
  return "Unknown";
}

std::string Error::describe() const {
  std::string out(to_string(code_));
  if (!message_.empty()) {
    out.append(": ");
    out.append(message_);
  }
  if (!context_.empty()) {
    out.append(" [");
    out.append(context_);
    out.append("]");
  }
  return out;
}

}  // namespace sol::coherence
