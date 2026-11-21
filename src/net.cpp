#include "fmtrans/net.hpp"

#include "fmtrans/sha1.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <sstream>
#include <thread>

namespace fmtrans::net {
namespace {

// Upper bound for a single request body (plan: limit request size).
constexpr size_t kMaxBodyBytes = 64 * 1024 * 1024;

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string trim(const std::string& value) {
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
  return value.substr(begin, end - begin);
}

void sendFrame(int fd, int opcode, const std::string& payload) {
  std::string frame;
  frame.push_back(static_cast<char>(0x80 | opcode));  // FIN + opcode
  const size_t len = payload.size();
  if (len < 126) {
    frame.push_back(static_cast<char>(len));
  } else if (len < 65536) {
    frame.push_back(static_cast<char>(126));
    frame.push_back(static_cast<char>((len >> 8) & 0xff));
    frame.push_back(static_cast<char>(len & 0xff));
  } else {
    frame.push_back(static_cast<char>(127));
    for (int i = 7; i >= 0; --i) {
      frame.push_back(static_cast<char>((static_cast<uint64_t>(len) >> (i * 8)) & 0xff));
    }
  }
  frame += payload;
  ::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
}

}  // namespace

std::string urlDecode(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (size_t i = 0; i < value.size(); ++i) {
    if (value[i] == '+') {
      out.push_back(' ');
    } else if (value[i] == '%' && i + 2 < value.size()) {
      const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = hex(value[i + 1]);
      const int lo = hex(value[i + 2]);
      if (hi >= 0 && lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
      } else {
        out.push_back(value[i]);
      }
    } else {
      out.push_back(value[i]);
    }
  }
  return out;
}

std::map<std::string, std::string> parseQuery(const std::string& raw) {
  std::map<std::string, std::string> result;
  std::stringstream stream(raw);
  std::string pair;
  while (std::getline(stream, pair, '&')) {
    if (pair.empty()) continue;
    const size_t eq = pair.find('=');
    if (eq == std::string::npos) {
      result[urlDecode(pair)] = "";
    } else {
      result[urlDecode(pair.substr(0, eq))] = urlDecode(pair.substr(eq + 1));
    }
  }
  return result;
}

Socket::~Socket() { close(); }

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }

Socket& Socket::operator=(Socket&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = other.fd_;
    other.fd_ = -1;
  }
  return *this;
}

void Socket::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

int Socket::release() {
  const int fd = fd_;
  fd_ = -1;
  return fd;
}

bool Socket::setTimeouts(int timeoutMs) {
  if (fd_ < 0) return false;
  timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
  return ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0 &&
         ::setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == 0;
}

bool Socket::connectTo(const std::string& host, uint16_t port, int timeoutMs) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* result = nullptr;
  const std::string portStr = std::to_string(port);
  if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result) != 0) return false;

  bool connected = false;
  for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
    const int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) continue;
    const int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int rc = ::connect(fd, it->ai_addr, it->ai_addrlen);
    if (rc == 0) {
      connected = true;
    } else if (errno == EINPROGRESS) {
      pollfd pfd{fd, POLLOUT, 0};
      const int ready = ::poll(&pfd, 1, timeoutMs);
      if (ready > 0) {
        int error = 0;
        socklen_t len = sizeof(error);
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
        connected = error == 0;
      }
    }
    if (connected) {
      ::fcntl(fd, F_SETFL, flags);
      fd_ = fd;
      setTimeouts(timeoutMs);
      break;
    }
    ::close(fd);
  }
  ::freeaddrinfo(result);
  return connected;
}

bool Socket::sendAll(const std::string& data) {
  size_t done = 0;
  while (done < data.size()) {
    const ssize_t sent = ::send(fd_, data.data() + done, data.size() - done, MSG_NOSIGNAL);
    if (sent > 0) {
      done += static_cast<size_t>(sent);
    } else if (sent == 0 || errno != EINTR) {
      return false;
    }
  }
  return true;
}

bool Socket::recvSome(std::string& out, size_t maxBytes) {
  char buffer[65536];
  const size_t want = std::min(maxBytes, sizeof(buffer));
  const ssize_t got = ::recv(fd_, buffer, want, MSG_NOSIGNAL);
  if (got > 0) {
    out.append(buffer, static_cast<size_t>(got));
    return true;
  }
  return false;
}

std::string HttpRequest::header(const std::string& key) const {
  const auto it = headers.find(lower(key));
  return it == headers.end() ? std::string() : it->second;
}

std::string HttpResponse::serialize() const {
  std::string reasonText = reason;
  if (reasonText.empty()) {
    switch (status) {
      case 200: reasonText = "OK"; break;
      case 201: reasonText = "Created"; break;
      case 400: reasonText = "Bad Request"; break;
      case 401: reasonText = "Unauthorized"; break;
      case 403: reasonText = "Forbidden"; break;
      case 404: reasonText = "Not Found"; break;
      case 405: reasonText = "Method Not Allowed"; break;
      case 409: reasonText = "Conflict"; break;
      case 413: reasonText = "Payload Too Large"; break;
      case 500: reasonText = "Internal Server Error"; break;
      default: reasonText = "Unknown";
    }
  }
  std::ostringstream out;
  out << "HTTP/1.1 " << status << " " << reasonText << "\r\n";
  if (headers.find("content-type") == headers.end()) {
    out << "Content-Type: text/plain; charset=utf-8\r\n";
  }
  for (const auto& [key, value] : headers) {
    out << key << ": " << value << "\r\n";
  }
  out << "Content-Length: " << body.size() << "\r\n";
  out << "Connection: close\r\n\r\n";
  out << body;
  return out.str();
}

HttpResponse HttpResponse::json(int status, const std::string& body) {
  HttpResponse response;
  response.status = status;
  response.body = body;
  response.headers["content-type"] = "application/json; charset=utf-8";
  return response;
}

HttpResponse HttpResponse::text(int status, const std::string& body) {
  HttpResponse response;
  response.status = status;
  response.body = body;
  response.headers["content-type"] = "text/plain; charset=utf-8";
  return response;
}

HttpResponse HttpResponse::bytes(int status, const std::string& body, const std::string& mime) {
  HttpResponse response;
  response.status = status;
  response.body = body;
  response.headers["content-type"] = mime;
  return response;
}

namespace {

std::optional<size_t> contentLength(const std::map<std::string, std::string>& headers) {
  const auto it = headers.find("content-length");
  if (it == headers.end()) return 0;
  try {
    return static_cast<size_t>(std::stoull(it->second));
  } catch (...) {
    return std::nullopt;
  }
}

bool readHeaders(Socket& socket, std::string& buffer) {
  while (buffer.find("\r\n\r\n") == std::string::npos) {
    if (!socket.recvSome(buffer)) return false;
    if (buffer.size() > 16 * 1024 * 1024) return false;
  }
  return true;
}

bool finishBody(Socket& socket, std::string& buffer, size_t headerEnd, size_t needed) {
  if (buffer.size() < headerEnd + 4) return false;
  while (buffer.size() - (headerEnd + 4) < needed) {
    std::string chunk;
    if (!socket.recvSome(chunk)) return false;
    buffer += chunk;
  }
  return true;
}

}  // namespace

std::optional<HttpRequest> readRequest(Socket& socket, int timeoutMs) {
  socket.setTimeouts(timeoutMs);
  std::string buffer;
  if (!readHeaders(socket, buffer)) return std::nullopt;
  const size_t headerEnd = buffer.find("\r\n\r\n");
  std::stringstream head(buffer.substr(0, headerEnd));

  HttpRequest request;
  if (!(head >> request.method >> request.path >> request.version)) return std::nullopt;

  std::string line;
  while (std::getline(head, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    request.headers[lower(line.substr(0, colon))] = trim(line.substr(colon + 1));
  }

  const size_t question = request.path.find('?');
  if (question != std::string::npos) {
    request.query = parseQuery(request.path.substr(question + 1));
    request.path = request.path.substr(0, question);
  }

  const auto length = contentLength(request.headers);
  if (!length) return std::nullopt;
  if (*length > kMaxBodyBytes) {
    request.tooLarge = true;
    return request;
  }
  if (!finishBody(socket, buffer, headerEnd, *length)) return std::nullopt;
  request.body = buffer.substr(headerEnd + 4, *length);
  request.extra = buffer.substr(headerEnd + 4 + *length);
  return request;
}

std::optional<HttpResponse> readResponse(Socket& socket, int timeoutMs) {
  socket.setTimeouts(timeoutMs);
  std::string buffer;
  if (!readHeaders(socket, buffer)) return std::nullopt;
  const size_t headerEnd = buffer.find("\r\n\r\n");
  std::stringstream head(buffer.substr(0, headerEnd));

  HttpResponse response;
  std::string version;
  if (!(head >> version >> response.status)) return std::nullopt;
  std::string line;
  while (std::getline(head, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    response.headers[lower(line.substr(0, colon))] = trim(line.substr(colon + 1));
  }

  const auto length = contentLength(response.headers);
  if (!length) return std::nullopt;
  if (!finishBody(socket, buffer, headerEnd, *length)) return std::nullopt;
  response.body = buffer.substr(headerEnd + 4, *length);
  return response;
}

HttpClient::Result HttpClient::request(const std::string& method, const std::string& host,
                                       uint16_t port, const std::string& path,
                                       const std::map<std::string, std::string>& headers,
                                       const std::string& body, int timeoutMs) {
  Result result;
  Socket socket;
  if (!socket.connectTo(host, port, timeoutMs)) {
    result.error = "connect failed: " + host + ":" + std::to_string(port);
    return result;
  }
  std::string raw = method + " " + path + " HTTP/1.1\r\n";
  raw += "Host: " + host + ":" + std::to_string(port) + "\r\n";
  for (const auto& [key, value] : headers) {
    raw += key + ": " + value + "\r\n";
  }
  raw += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  raw += "Connection: close\r\n\r\n";
  raw += body;
  if (!socket.sendAll(raw)) {
    result.error = "send failed";
    return result;
  }
  const auto response = readResponse(socket, timeoutMs);
  if (!response) {
    result.error = "read response failed";
    return result;
  }
  result.ok = true;
  result.response = *response;
  return result;
}

HttpServer::HttpServer(uint16_t port, Handler handler, WsHandler wsHandler,
                       std::string host)
    : port_(port), host_(std::move(host)), handler_(std::move(handler)),
      wsHandler_(std::move(wsHandler)) {}

bool HttpServer::bind() {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* result = nullptr;
  const std::string service = std::to_string(port_);
  if (::getaddrinfo(host_.empty() ? nullptr : host_.c_str(), service.c_str(), &hints,
                    &result) != 0) {
    return false;
  }
  bool bound = false;
  for (addrinfo* it = result; it != nullptr; it = it->ai_next) {
    const int fd = ::socket(it->ai_family, it->ai_socktype, it->ai_protocol);
    if (fd < 0) continue;
    int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (::bind(fd, it->ai_addr, it->ai_addrlen) == 0 && ::listen(fd, 128) == 0) {
      listenFd_ = fd;
      bound = true;
      break;
    }
    ::close(fd);
  }
  ::freeaddrinfo(result);
  if (!bound) return false;
  timeval tv{0, 200000};
  ::setsockopt(listenFd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return true;
}

bool HttpServer::serve() {
  const Handler handler = handler_;
  const WsHandler wsHandler = wsHandler_;
  while (!stop_) {
    sockaddr_in peer{};
    socklen_t peerLen = sizeof(peer);
    const int fd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&peer), &peerLen);
    if (fd < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
      break;
    }
    char peerBuf[INET_ADDRSTRLEN] = {0};
    ::inet_ntop(AF_INET, &peer.sin_addr, peerBuf, sizeof(peerBuf));
    std::thread([handler, wsHandler, fd, peerBuf]() {
      Socket socket(fd);
      auto request = readRequest(socket);
      if (!request) return;
      request->peer = peerBuf;
      if (request->tooLarge) {
        socket.sendAll(HttpResponse::text(413, "payload too large").serialize());
        return;
      }
      if (wsHandler && request->path == "/ws") {
        wsHandler(socket.release(), *request);
        return;
      }
      const HttpResponse response = handler(*request);
      socket.sendAll(response.serialize());
    }).detach();
  }
  return true;
}

bool HttpServer::start() {
  if (!bind()) return false;
  return serve();
}

void HttpServer::stop() {
  stop_ = true;
  if (listenFd_ >= 0) {
    ::shutdown(listenFd_, SHUT_RDWR);
    ::close(listenFd_);
    listenFd_ = -1;
  }
}

namespace ws {

std::string acceptKey(const std::string& clientKey) {
  return base64Encode(sha1Raw(clientKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"));
}

void sendText(int fd, const std::string& text) { sendFrame(fd, 0x1, text); }

void sendPing(int fd) { sendFrame(fd, 0x9, ""); }

namespace {

bool recvInto(int fd, std::string& buffer) {
  char chunk[65536];
  const ssize_t got = ::recv(fd, chunk, sizeof(chunk), MSG_NOSIGNAL);
  if (got > 0) {
    buffer.append(chunk, static_cast<size_t>(got));
    return true;
  }
  if (got < 0 && errno == EINTR) return true;
  return false;
}

}  // namespace

Frame recvFrame(int fd, std::string& buffer) {
  while (true) {
    if (buffer.size() < 2) {
      if (!recvInto(fd, buffer)) {
        Frame frame;
        frame.close = true;
        return frame;
      }
      continue;
    }
    const auto* head = reinterpret_cast<const unsigned char*>(buffer.data());
    const int opcode = head[0] & 0x0f;
    const bool masked = (head[1] & 0x80) != 0;
    uint64_t length = head[1] & 0x7f;
    size_t header = 2;
    if (length == 126) {
      if (buffer.size() < 4) {
        if (!recvInto(fd, buffer)) {
          Frame frame;
          frame.close = true;
          return frame;
        }
        continue;
      }
      length = (static_cast<uint64_t>(static_cast<unsigned char>(buffer[2])) << 8) |
              static_cast<unsigned char>(buffer[3]);
      header = 4;
    } else if (length == 127) {
      if (buffer.size() < 10) {
        if (!recvInto(fd, buffer)) {
          Frame frame;
          frame.close = true;
          return frame;
        }
        continue;
      }
      length = 0;
      for (size_t i = 0; i < 8; ++i) {
        length = (length << 8) | static_cast<unsigned char>(buffer[2 + i]);
      }
      header = 10;
    }
    if (length > 64 * 1024 * 1024) {
      Frame frame;
      frame.close = true;
      return frame;
    }
    const size_t maskBytes = masked ? 4 : 0;
    const size_t total = header + maskBytes + static_cast<size_t>(length);
    if (buffer.size() < total) {
      if (!recvInto(fd, buffer)) {
        Frame frame;
        frame.close = true;
        return frame;
      }
      continue;
    }
    Frame frame;
    frame.opcode = opcode;
    frame.payload.assign(buffer.substr(header + maskBytes, static_cast<size_t>(length)));
    if (masked) {
      for (size_t i = 0; i < frame.payload.size(); ++i) {
        frame.payload[i] =
            static_cast<char>(frame.payload[i] ^ buffer[header + (i % 4)]);
      }
    }
    buffer.erase(0, total);
    if (opcode == 0x9) {
      sendFrame(fd, 0xA, frame.payload);
      continue;
    }
    if (opcode == 0x8) frame.close = true;
    return frame;
  }
}

Frame recvFrame(int fd) {
  std::string buffer;
  return recvFrame(fd, buffer);
}

}  // namespace ws
}  // namespace fmtrans::net
