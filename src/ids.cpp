// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "coherence/ids.hpp"

#include <cctype>

namespace sol::coherence {

Status validate_identity_token(std::string_view token) {
  if (token.empty()) {
    return fail(ErrorCode::InvalidArgument, "identity token is empty");
  }
  if (token.size() > Limits::kMaxNameLength) {
    return fail(ErrorCode::TooLarge, "identity token exceeds maximum length",
                std::string("length=") + std::to_string(token.size()));
  }
  for (char raw : token) {
    const unsigned char c = static_cast<unsigned char>(raw);
    if (c >= 0x80u) {
      return fail(ErrorCode::InvalidArgument,
                  "identity token must be ASCII",
                  std::string("byte=0x") + std::to_string(static_cast<unsigned>(c)));
    }
    if (std::isalnum(c) != 0) {
      continue;
    }
    switch (raw) {
      case '.': case '_': case '-': case ':': case '@': case '#': case '+':
        continue;
      default:
        return fail(ErrorCode::InvalidArgument, "identity token contains a forbidden character",
                    std::string("char='") + raw + "'");
    }
  }
  return Status();
}

}  // namespace sol::coherence
