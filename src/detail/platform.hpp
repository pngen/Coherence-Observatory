// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.
//
// Platform primitives: bounded file I/O with atomic replacement, and real
// TCP sockets.  Only this translation unit touches operating-system APIs.
// Not installed.

#ifndef COHERENCE_SRC_DETAIL_PLATFORM_HPP
#define COHERENCE_SRC_DETAIL_PLATFORM_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "coherence/error.hpp"

namespace sol::coherence::detail {

// ---- Files -------------------------------------------------------------

/// Reads at most \p max_bytes from \p path.
///
/// The file is rejected outright when it is larger than the bound or cannot be
/// opened; nothing is partially read into the result.
Result<std::vector<std::uint8_t>> read_file_bounded(const std::filesystem::path& path,
                                                    std::uint64_t max_bytes);

/// Writes \p data to \p path through a sibling temporary file, then replaces
/// the destination atomically.  The temporary file is removed on failure.
Status write_file_atomic(const std::filesystem::path& path, const std::uint8_t* data,
                         std::size_t size);

Status remove_file(const std::filesystem::path& path);

// ---- Sockets -----------------------------------------------------------

/// Owning socket handle.
class Socket {
 public:
  Socket() = default;
  ~Socket();

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  bool valid() const noexcept;
  void close() noexcept;
  std::uintptr_t native() const noexcept { return handle_; }

 private:
  friend Result<Socket> tcp_connect(const std::string&, std::uint16_t, std::uint32_t);
  friend Result<Socket> tcp_accept(const Socket&);
  friend Result<Socket> tcp_listen(const std::string&, std::uint16_t, std::size_t);
  explicit Socket(std::uintptr_t handle) : handle_(handle) {}

  std::uintptr_t handle_ = static_cast<std::uintptr_t>(~static_cast<std::uintptr_t>(0));
};

/// Initializes the platform networking stack once per process.
Status network_init();

/// Connects to \p host : \p port with a bounded connect timeout.
Result<Socket> tcp_connect(const std::string& host, std::uint16_t port,
                           std::uint32_t timeout_ms);

/// Binds and listens.  A port of 0 selects an ephemeral port.
Result<Socket> tcp_listen(const std::string& host, std::uint16_t port, std::size_t backlog);

/// Accepts one connection.  Returns ConnectionClosed when the listener is gone.
Result<Socket> tcp_accept(const Socket& listener);

/// Local port bound by \p socket.
Result<std::uint16_t> socket_local_port(const Socket& socket);

/// Human-readable peer description ("127.0.0.1:52344").
Result<std::string> socket_peer_text(const Socket& socket);

/// Sets receive/send timeouts in milliseconds; 0 disables.
Status socket_set_timeouts(const Socket& socket, std::uint32_t receive_ms,
                           std::uint32_t send_ms);
Status socket_set_nodelay(const Socket& socket);
Status socket_set_reuse_address(const Socket& socket);

/// Sends the whole buffer, or fails.
Status socket_send_all(const Socket& socket, const std::uint8_t* data, std::size_t size);

/// Receives at least one byte, or reports the number read (0 = orderly close).
Result<std::size_t> socket_recv_some(const Socket& socket, std::uint8_t* buffer,
                                     std::size_t capacity);

/// Waits until any socket in \p sockets is readable or the timeout elapses.
///
/// \p timeout_ms of 0 polls.  Returns the number of ready sockets.
Result<std::size_t> socket_wait_readable(const std::vector<const Socket*>& sockets,
                                         std::uint32_t timeout_ms);

/// True when p socket has readable data within p timeout_ms.
Result<bool> socket_is_readable(const Socket& socket, std::uint32_t timeout_ms);

/// True when p code denotes a would-block / interrupted condition.
bool socket_error_is_transient(int code) noexcept;

/// Last socket error code.
int socket_last_error() noexcept;

}  // namespace sol::coherence::detail

#endif  // COHERENCE_SRC_DETAIL_PLATFORM_HPP
