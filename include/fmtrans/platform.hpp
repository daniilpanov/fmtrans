#pragma once

// Cross-platform abstraction layer.
// Linux/POSIX implementation lives in src/platform.cpp.
// Windows: trivial stubs are inlined below; a full Windows port requires
// Winsock, CreateProcess, and native Win32 file APIs (see PLAN.md §11, item 3).

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using pid_t = int;
#else
#include <sys/types.h>
#endif

namespace fmtrans::platform {

// Socket type. On POSIX this is an int fd; on Windows a SOCKET handle.
using SocketHandle = intptr_t;
constexpr SocketHandle kInvalidSocket = -1;

// ---------- Socket operations ----------

int lastSocketError();
int wouldBlockError();
SocketHandle createSocket();
bool closeSocket(SocketHandle fd);
bool setSocketTimeouts(SocketHandle fd, int timeoutMs);
bool setSocketNonBlocking(SocketHandle fd, bool nonBlocking);
std::string socketErrorString(int err);

// ---------- Process operations ----------

bool processExists(pid_t pid);
std::string processCmdline(pid_t pid);

// ---------- File operations ----------

bool fileExists(const std::string& path);
bool createDirectories(const std::string& path);
bool setFilePermissions(const std::string& path, int mode);

}  // namespace fmtrans::platform

#ifdef _WIN32

// Windows stub implementations (minimal; not a functional port).
namespace fmtrans::platform {

inline int lastSocketError() { return WSAGetLastError(); }
inline int wouldBlockError() { return WSAEWOULDBLOCK; }
inline SocketHandle createSocket() { return kInvalidSocket; }
inline bool closeSocket(SocketHandle) { return false; }
inline bool setSocketTimeouts(SocketHandle, int) { return false; }
inline bool setSocketNonBlocking(SocketHandle, bool) { return false; }
inline std::string socketErrorString(int err) {
  return "socket error " + std::to_string(err);
}
inline bool processExists(pid_t) { return false; }
inline std::string processCmdline(pid_t) { return ""; }
inline bool fileExists(const std::string& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}
inline bool createDirectories(const std::string& path) {
  std::error_code ec;
  return std::filesystem::create_directories(path, ec);
}
inline bool setFilePermissions(const std::string&, int) { return false; }

}  // namespace fmtrans::platform

#else

// Declarations for the POSIX implementation in src/platform.cpp.
namespace fmtrans::platform {

int lastSocketError();
int wouldBlockError();
SocketHandle createSocket();
bool closeSocket(SocketHandle fd);
bool setSocketTimeouts(SocketHandle fd, int timeoutMs);
bool setSocketNonBlocking(SocketHandle fd, bool nonBlocking);
std::string socketErrorString(int err);
bool processExists(pid_t pid);
std::string processCmdline(pid_t pid);
bool fileExists(const std::string& path);
bool createDirectories(const std::string& path);
bool setFilePermissions(const std::string& path, int mode);

}  // namespace fmtrans::platform

#endif
