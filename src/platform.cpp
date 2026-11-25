#include "fmtrans/platform.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

namespace fmtrans::platform {

int lastSocketError() { return errno; }

int wouldBlockError() { return EWOULDBLOCK; }

SocketHandle createSocket() {
  return ::socket(AF_INET, SOCK_STREAM, 0);
}

bool closeSocket(SocketHandle fd) {
  if (fd < 0) return true;
  return ::close(static_cast<int>(fd)) == 0;
}

bool setSocketTimeouts(SocketHandle fd, int timeoutMs) {
  if (fd < 0) return false;
  timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
  return ::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_RCVTIMEO, &tv,
                      sizeof(tv)) == 0 &&
         ::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_SNDTIMEO, &tv,
                      sizeof(tv)) == 0;
}

bool setSocketNonBlocking(SocketHandle fd, bool nonBlocking) {
  if (fd < 0) return false;
  const int flags = ::fcntl(static_cast<int>(fd), F_GETFL, 0);
  if (flags < 0) return false;
  const int newFlags =
      nonBlocking ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
  return ::fcntl(static_cast<int>(fd), F_SETFL, newFlags) == 0;
}

std::string socketErrorString(int err) {
  return std::strerror(err);
}

bool processExists(pid_t pid) {
  if (pid <= 0) return false;
  return ::kill(pid, 0) == 0;
}

std::string processCmdline(pid_t pid) {
  std::ifstream input("/proc/" + std::to_string(pid) + "/cmdline",
                      std::ios::binary);
  if (!input) return "";
  return std::string((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
}

bool fileExists(const std::string& path) {
  std::error_code ec;
  return std::filesystem::exists(path, ec);
}

bool createDirectories(const std::string& path) {
  std::error_code ec;
  return std::filesystem::create_directories(path, ec);
}

bool setFilePermissions(const std::string& path, int mode) {
  return ::chmod(path.c_str(), mode) == 0;
}

}  // namespace fmtrans::platform
