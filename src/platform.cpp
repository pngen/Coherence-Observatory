// Coherence Observatory 1.0.0
// Copyright 2026 Summon Software Labs.
// Apache License 2.0 -- see LICENSE.

#include "detail/platform.hpp"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "text_util.hpp"

#if defined(_WIN32)
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace sol::coherence::detail {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr std::uintptr_t kInvalidHandle = static_cast<std::uintptr_t>(INVALID_SOCKET);
constexpr NativeSocket kInvalidNative = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr std::uintptr_t kInvalidHandle = static_cast<std::uintptr_t>(-1);
constexpr NativeSocket kInvalidNative = -1;
#endif

NativeSocket native_of(const Socket& socket) noexcept {
  return static_cast<NativeSocket>(socket.native());
}

bool native_valid(NativeSocket handle) noexcept { return handle != kInvalidNative; }

}  // namespace

// ---- Files -------------------------------------------------------------

Result<std::vector<std::uint8_t>> read_file_bounded(const std::filesystem::path& path,
                                                    std::uint64_t max_bytes) {
  std::error_code ec;
  const std::uintmax_t size = std::filesystem::file_size(path, ec);
  if (ec) {
    return fail_as<std::vector<std::uint8_t>>(ErrorCode::NotFound, "cannot stat file",
                                              path.string());
  }
  if (size > static_cast<std::uintmax_t>(max_bytes)) {
    return fail_as<std::vector<std::uint8_t>>(ErrorCode::TooLarge,
                                              "file exceeds the accepted size bound",
                                              path.string());
  }
  std::FILE* file = nullptr;
#if defined(_WIN32)
  if (fopen_s(&file, path.string().c_str(), "rb") != 0) {
    file = nullptr;
  }
#else
  file = std::fopen(path.string().c_str(), "rb");
#endif
  if (file == nullptr) {
    return fail_as<std::vector<std::uint8_t>>(ErrorCode::IoError, "cannot open file",
                                              path.string());
  }
  std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
  std::size_t read_total = 0;
  while (read_total < data.size()) {
    const std::size_t got =
        std::fread(data.data() + read_total, 1, data.size() - read_total, file);
    if (got == 0) {
      break;
    }
    read_total += got;
  }
  const bool short_read = read_total != data.size();
  std::fclose(file);
  if (short_read) {
    return fail_as<std::vector<std::uint8_t>>(ErrorCode::Truncated,
                                              "file ended before its declared length",
                                              path.string());
  }
  return Result<std::vector<std::uint8_t>>(std::move(data));
}

Status remove_file(const std::filesystem::path& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (ec) {
    return fail(ErrorCode::IoError, "cannot remove file", path.string());
  }
  return Status();
}

Status write_file_atomic(const std::filesystem::path& path, const std::uint8_t* data,
                         std::size_t size) {
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  {
    std::FILE* file = nullptr;
#if defined(_WIN32)
    if (fopen_s(&file, temporary.string().c_str(), "wb") != 0) {
      file = nullptr;
    }
#else
    file = std::fopen(temporary.string().c_str(), "wb");
#endif
    if (file == nullptr) {
      return fail(ErrorCode::IoError, "cannot open temporary state file", temporary.string());
    }
    std::size_t written = 0;
    while (written < size) {
      const std::size_t put = std::fwrite(data + written, 1, size - written, file);
      if (put == 0) {
        break;
      }
      written += put;
    }
    const bool short_write = written != size;
    const int flush_result = std::fflush(file);
    std::fclose(file);
    if (short_write || flush_result != 0) {
      remove_file(temporary);
      return fail(ErrorCode::IoError, "short write to temporary state file",
                  temporary.string());
    }
  }

#if defined(_WIN32)
  if (MoveFileExW(temporary.wstring().c_str(), path.wstring().c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    remove_file(temporary);
    return fail(ErrorCode::IoError, "atomic replacement failed", path.string());
  }
#else
  std::error_code ec;
  std::filesystem::rename(temporary, path, ec);
  if (ec) {
    remove_file(temporary);
    return fail(ErrorCode::IoError, "atomic replacement failed", path.string());
  }
#endif
  return Status();
}

// ---- Sockets -----------------------------------------------------------

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : handle_(other.handle_) {
  other.handle_ = kInvalidHandle;
}

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    handle_ = other.handle_;
    other.handle_ = kInvalidHandle;
  }
  return *this;
}

bool Socket::valid() const noexcept { return handle_ != kInvalidHandle; }

void Socket::close() noexcept {
  if (!valid()) {
    return;
  }
#if defined(_WIN32)
  ::closesocket(static_cast<SOCKET>(handle_));
#else
  ::close(static_cast<int>(handle_));
#endif
  handle_ = kInvalidHandle;
}

Status network_init() {
#if defined(_WIN32)
  static std::once_flag once;
  static int startup_result = 0;
  std::call_once(once, []() {
    WSADATA data{};
    startup_result = WSAStartup(MAKEWORD(2, 2), &data);
  });
  if (startup_result != 0) {
    return fail(ErrorCode::IoError, "WSAStartup failed", format_i64(startup_result));
  }
#endif
  return Status();
}

int socket_last_error() noexcept {
#if defined(_WIN32)
  return WSAGetLastError();
#else
  return errno;
#endif
}

bool socket_error_is_transient(int code) noexcept {
#if defined(_WIN32)
  return code == WSAEWOULDBLOCK || code == WSAEINTR || code == WSAETIMEDOUT;
#else
  return code == EWOULDBLOCK || code == EAGAIN || code == EINTR;
#endif
}

Result<Socket> tcp_connect(const std::string& host, std::uint16_t port,
                           std::uint32_t timeout_ms) {
  const Status initialized = network_init();
  if (!initialized.ok()) {
    return Result<Socket>(initialized.error());
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* results = nullptr;
  const std::string port_text = format_u64(port);
  const int resolved = ::getaddrinfo(host.c_str(), port_text.c_str(), &hints, &results);
  if (resolved != 0 || results == nullptr) {
    return fail_as<Socket>(ErrorCode::IoError, "cannot resolve host", host);
  }
  Socket socket;
  bool connected = false;
  for (addrinfo* candidate = results; candidate != nullptr; candidate = candidate->ai_next) {
    const NativeSocket handle =
        ::socket(candidate->ai_family, candidate->ai_socktype, candidate->ai_protocol);
    if (!native_valid(handle)) {
      continue;
    }
    Socket attempt(static_cast<std::uintptr_t>(handle));
    if (::connect(handle, candidate->ai_addr, static_cast<int>(candidate->ai_addrlen)) == 0) {
      socket = std::move(attempt);
      connected = true;
      break;
    }
  }
  ::freeaddrinfo(results);
  if (!connected) {
    return fail_as<Socket>(ErrorCode::IoError, "cannot connect to coordinator",
                           host + ":" + port_text);
  }
  if (timeout_ms != 0) {
    const Status timeouts = socket_set_timeouts(socket, timeout_ms, timeout_ms);
    if (!timeouts.ok()) {
      return Result<Socket>(timeouts.error());
    }
  }
  socket_set_nodelay(socket);
  return Result<Socket>(std::move(socket));
}

Result<Socket> tcp_listen(const std::string& host, std::uint16_t port, std::size_t backlog) {
  const Status initialized = network_init();
  if (!initialized.ok()) {
    return Result<Socket>(initialized.error());
  }
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* results = nullptr;
  const std::string port_text = format_u64(port);
  const char* node = host.empty() ? nullptr : host.c_str();
  const int resolved = ::getaddrinfo(node, port_text.c_str(), &hints, &results);
  if (resolved != 0 || results == nullptr) {
    return fail_as<Socket>(ErrorCode::IoError, "cannot resolve bind address", host);
  }
  const NativeSocket handle =
      ::socket(results->ai_family, results->ai_socktype, results->ai_protocol);
  if (!native_valid(handle)) {
    ::freeaddrinfo(results);
    return fail_as<Socket>(ErrorCode::IoError, "cannot create listening socket", host);
  }
  Socket socket(static_cast<std::uintptr_t>(handle));
  socket_set_reuse_address(socket);
  if (::bind(handle, results->ai_addr, static_cast<int>(results->ai_addrlen)) != 0) {
    ::freeaddrinfo(results);
    return fail_as<Socket>(ErrorCode::IoError, "cannot bind listening socket", host);
  }
  ::freeaddrinfo(results);
  if (::listen(handle, static_cast<int>(backlog)) != 0) {
    return fail_as<Socket>(ErrorCode::IoError, "cannot listen", host);
  }
  return Result<Socket>(std::move(socket));
}

Result<Socket> tcp_accept(const Socket& listener) {
  if (!listener.valid()) {
    return fail_as<Socket>(ErrorCode::ConnectionClosed, "listener is closed");
  }
  const NativeSocket handle = ::accept(native_of(listener), nullptr, nullptr);
  if (!native_valid(handle)) {
    return fail_as<Socket>(ErrorCode::IoError, "accept failed",
                           format_i64(socket_last_error()));
  }
  Socket socket(static_cast<std::uintptr_t>(handle));
  socket_set_nodelay(socket);
  return Result<Socket>(std::move(socket));
}

Result<std::uint16_t> socket_local_port(const Socket& socket) {
  if (!socket.valid()) {
    return fail_as<std::uint16_t>(ErrorCode::InvalidArgument, "socket is not open");
  }
  sockaddr_storage address{};
  int length = static_cast<int>(sizeof(address));
  if (::getsockname(native_of(socket), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return fail_as<std::uint16_t>(ErrorCode::IoError, "getsockname failed");
  }
  if (address.ss_family == AF_INET) {
    const auto* inet = reinterpret_cast<const sockaddr_in*>(&address);
    return Result<std::uint16_t>(ntohs(inet->sin_port));
  }
  return fail_as<std::uint16_t>(ErrorCode::Internal, "unexpected address family");
}

Result<std::string> socket_peer_text(const Socket& socket) {
  if (!socket.valid()) {
    return fail_as<std::string>(ErrorCode::InvalidArgument, "socket is not open");
  }
  sockaddr_storage address{};
  int length = static_cast<int>(sizeof(address));
  if (::getpeername(native_of(socket), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
    return fail_as<std::string>(ErrorCode::IoError, "getpeername failed");
  }
  std::array<char, 64> buffer{};
  if (address.ss_family == AF_INET) {
    const auto* inet = reinterpret_cast<const sockaddr_in*>(&address);
    const char* converted =
        ::inet_ntop(AF_INET, &inet->sin_addr, buffer.data(), static_cast<socklen_t>(buffer.size()));
    if (converted == nullptr) {
      return fail_as<std::string>(ErrorCode::IoError, "inet_ntop failed");
    }
    return Result<std::string>(std::string(buffer.data()) + ":" +
                               format_u64(ntohs(inet->sin_port)));
  }
  return Result<std::string>(std::string("unknown"));
}

Status socket_set_timeouts(const Socket& socket, std::uint32_t receive_ms,
                           std::uint32_t send_ms) {
  if (!socket.valid()) {
    return fail(ErrorCode::InvalidArgument, "socket is not open");
  }
#if defined(_WIN32)
  const DWORD receive = static_cast<DWORD>(receive_ms);
  const DWORD send = static_cast<DWORD>(send_ms);
#else
  timeval receive{};
  receive.tv_sec = static_cast<long>(receive_ms / 1000);
  receive.tv_usec = static_cast<long>((receive_ms % 1000) * 1000);
  timeval send{};
  send.tv_sec = static_cast<long>(send_ms / 1000);
  send.tv_usec = static_cast<long>((send_ms % 1000) * 1000);
#endif
  const NativeSocket handle = native_of(socket);
  if (::setsockopt(handle, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&receive),
                   static_cast<int>(sizeof(receive))) != 0) {
    return fail(ErrorCode::IoError, "cannot set receive timeout",
                format_i64(socket_last_error()));
  }
  if (::setsockopt(handle, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&send),
                   static_cast<int>(sizeof(send))) != 0) {
    return fail(ErrorCode::IoError, "cannot set send timeout",
                format_i64(socket_last_error()));
  }
  return Status();
}

Status socket_set_nodelay(const Socket& socket) {
  if (!socket.valid()) {
    return fail(ErrorCode::InvalidArgument, "socket is not open");
  }
  const int enabled = 1;
  ::setsockopt(native_of(socket), IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&enabled), static_cast<int>(sizeof(enabled)));
  return Status();
}

Status socket_set_reuse_address(const Socket& socket) {
  if (!socket.valid()) {
    return fail(ErrorCode::InvalidArgument, "socket is not open");
  }
  const int enabled = 1;
  ::setsockopt(native_of(socket), SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&enabled), static_cast<int>(sizeof(enabled)));
  return Status();
}

Status socket_send_all(const Socket& socket, const std::uint8_t* data, std::size_t size) {
  if (!socket.valid()) {
    return fail(ErrorCode::ConnectionClosed, "socket is closed");
  }
  const NativeSocket handle = native_of(socket);
  constexpr std::size_t kChunk = 1u << 20;
  std::size_t sent_total = 0;
  while (sent_total < size) {
    const std::size_t remaining = size - sent_total;
    const std::size_t chunk = remaining > kChunk ? kChunk : remaining;
    const int sent = ::send(handle, reinterpret_cast<const char*>(data + sent_total),
                            static_cast<int>(chunk), 0);
    if (sent <= 0) {
      const int code = socket_last_error();
      if (socket_error_is_transient(code)) {
        return fail(ErrorCode::IoError, "send timed out",
                    std::string("transient=") + format_i64(code));
      }
      return fail(ErrorCode::ConnectionClosed, "send failed", format_i64(code));
    }
    sent_total += static_cast<std::size_t>(sent);
  }
  return Status();
}

Result<std::size_t> socket_recv_some(const Socket& socket, std::uint8_t* buffer,
                                     std::size_t capacity) {
  if (!socket.valid()) {
    return fail_as<std::size_t>(ErrorCode::ConnectionClosed, "socket is closed");
  }
  constexpr std::size_t kChunk = 1u << 20;
  const std::size_t chunk = capacity > kChunk ? kChunk : capacity;
  const int received = ::recv(native_of(socket), reinterpret_cast<char*>(buffer),
                              static_cast<int>(chunk), 0);
  if (received == 0) {
    return Result<std::size_t>(static_cast<std::size_t>(0));
  }
  if (received < 0) {
    const int code = socket_last_error();
    if (socket_error_is_transient(code)) {
      return fail_as<std::size_t>(ErrorCode::Busy, "receive timed out", format_i64(code));
    }
    return fail_as<std::size_t>(ErrorCode::ConnectionClosed, "receive failed",
                                format_i64(code));
  }
  return Result<std::size_t>(static_cast<std::size_t>(received));
}

Result<bool> socket_is_readable(const Socket& socket, std::uint32_t timeout_ms) {
  if (!socket.valid()) {
    return fail_as<bool>(ErrorCode::InvalidArgument, "socket is not open");
  }
  fd_set read_set;
  FD_ZERO(&read_set);
  const NativeSocket handle = native_of(socket);
  FD_SET(handle, &read_set);
  timeval timeout{};
  timeout.tv_sec = static_cast<long>(timeout_ms / 1000);
  timeout.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
  const int ready =
      ::select(static_cast<int>(handle) + 1, &read_set, nullptr, nullptr, &timeout);
  if (ready < 0) {
    const int code = socket_last_error();
    if (socket_error_is_transient(code)) {
      return Result<bool>(false);
    }
    return fail_as<bool>(ErrorCode::IoError, "select failed", format_i64(code));
  }
  return Result<bool>(ready > 0);
}

Result<std::size_t> socket_wait_readable(const std::vector<const Socket*>& sockets,
                                         std::uint32_t timeout_ms) {
  fd_set read_set;
  FD_ZERO(&read_set);
  NativeSocket highest = 0;
  std::size_t counted = 0;
  for (const Socket* socket : sockets) {
    if (socket == nullptr || !socket->valid()) {
      continue;
    }
    const NativeSocket handle = native_of(*socket);
    FD_SET(handle, &read_set);
    if (handle > highest) {
      highest = handle;
    }
    ++counted;
  }
  if (counted == 0) {
    return Result<std::size_t>(static_cast<std::size_t>(0));
  }
  timeval timeout{};
  timeout.tv_sec = static_cast<long>(timeout_ms / 1000);
  timeout.tv_usec = static_cast<long>((timeout_ms % 1000) * 1000);
  const int ready = ::select(static_cast<int>(highest) + 1, &read_set, nullptr, nullptr,
                             &timeout);
  if (ready < 0) {
    const int code = socket_last_error();
    if (socket_error_is_transient(code)) {
      return Result<std::size_t>(static_cast<std::size_t>(0));
    }
    return fail_as<std::size_t>(ErrorCode::IoError, "select failed", format_i64(code));
  }
  return Result<std::size_t>(static_cast<std::size_t>(ready));
}

}  // namespace sol::coherence::detail
