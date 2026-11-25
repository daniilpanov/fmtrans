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
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>

namespace fmtrans {

namespace fs_byte_reader {

ByteReader fromStream(std::istream& input) {
  return [&input](Bytes& out, size_t maxBytes) -> size_t {
    out.clear();
    out.resize(maxBytes);
    input.read(reinterpret_cast<char*>(out.data()),
               static_cast<std::streamsize>(maxBytes));
    const auto got = static_cast<size_t>(input.gcount());
    out.resize(got);
    return got;
  };
}

}  // namespace fs_byte_reader

}  // namespace fmtrans

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

void sendFrame(int fd, int opcode, const Bytes& payload) {
  Bytes frame;
  frame.push_back(static_cast<std::uint8_t>(0x80 | opcode));  // FIN + opcode
  const size_t len = payload.size();
  if (len < 126) {
    frame.push_back(static_cast<std::uint8_t>(len));
  } else if (len < 65536) {
    frame.push_back(static_cast<std::uint8_t>(126));
    frame.push_back(static_cast<std::uint8_t>((len >> 8) & 0xff));
    frame.push_back(static_cast<std::uint8_t>(len & 0xff));
  } else {
    frame.push_back(static_cast<std::uint8_t>(127));
    for (int i = 7; i >= 0; --i) {
      frame.push_back(
          static_cast<std::uint8_t>((static_cast<uint64_t>(len) >> (i * 8)) & 0xff));
    }
  }
  frame.insert(frame.end(), payload.begin(), payload.end());
  ::send(fd, reinterpret_cast<const char*>(frame.data()), frame.size(), MSG_NOSIGNAL);
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

bool Socket::sendAll(const Bytes& data) {
  size_t done = 0;
  while (done < data.size()) {
    const ssize_t sent =
        ::send(fd_, reinterpret_cast<const char*>(data.data()) + done,
               data.size() - done, MSG_NOSIGNAL);
    if (sent > 0) {
      done += static_cast<size_t>(sent);
    } else if (sent == 0 || errno != EINTR) {
      return false;
    }
  }
  return true;
}

bool Socket::sendAll(const std::string& data) { return sendAll(toBytes(data)); }

bool Socket::recvSome(Bytes& out, size_t maxBytes) {
  std::vector<std::uint8_t> buffer(std::min(maxBytes, static_cast<size_t>(65536)));
  const ssize_t got = ::recv(fd_, reinterpret_cast<char*>(buffer.data()), buffer.size(),
                             MSG_NOSIGNAL);
  if (got > 0) {
    out.insert(out.end(), buffer.begin(), buffer.begin() + got);
    return true;
  }
  return false;
}

std::string HttpRequest::header(const std::string& key) const {
  const auto it = headers.find(lower(key));
  return it == headers.end() ? std::string() : it->second;
}

Bytes HttpResponse::serializeHeaders() const {
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
  std::ostringstream head;
  head << "HTTP/1.1 " << status << " " << reasonText << "\r\n";
  if (headers.find("content-type") == headers.end()) {
    head << "Content-Type: text/plain; charset=utf-8\r\n";
  }
  for (const auto& [key, value] : headers) {
    head << key << ": " << value << "\r\n";
  }
  if (bodyStream) {
    // Streaming response: no Content-Length, close when done.
    head << "Connection: close\r\n\r\n";
  } else {
    head << "Content-Length: " << body.size() << "\r\n";
    head << "Connection: close\r\n\r\n";
  }
  return toBytes(head.str());
}

Bytes HttpResponse::serialize() const {
  Bytes result = serializeHeaders();
  if (!bodyStream) {
    result.insert(result.end(), body.begin(), body.end());
  }
  return result;
}

HttpResponse HttpResponse::json(int status, const std::string& body) {
  HttpResponse response;
  response.status = status;
  response.body = toBytes(body);
  response.headers["content-type"] = "application/json; charset=utf-8";
  return response;
}

HttpResponse HttpResponse::text(int status, const std::string& body) {
  HttpResponse response;
  response.status = status;
  response.body = toBytes(body);
  response.headers["content-type"] = "text/plain; charset=utf-8";
  return response;
}

HttpResponse HttpResponse::bytes(int status, const Bytes& body, const std::string& mime) {
  HttpResponse response;
  response.status = status;
  response.body = body;
  response.headers["content-type"] = mime;
  return response;
}

namespace {

std::optional<size_t> contentLength(const std::map<std::string, std::string>& headers) {
  const auto it = headers.find("content-length");
  if (it == headers.end()) return std::nullopt;
  try {
    return static_cast<size_t>(std::stoull(it->second));
  } catch (...) {
    return std::nullopt;
  }
}

}  // namespace

size_t headerEnd(const Bytes& buffer) {
  static const std::uint8_t needle[] = {'\r', '\n', '\r', '\n'};
  const auto it = std::search(buffer.begin(), buffer.end(), needle, needle + 4);
  return it == buffer.end() ? std::string::npos
                            : static_cast<size_t>(it - buffer.begin());
}

size_t lineEnd(const Bytes& buffer) {
  static const std::uint8_t needle[] = {'\r', '\n'};
  const auto it = std::search(buffer.begin(), buffer.end(), needle, needle + 2);
  return it == buffer.end() ? std::string::npos
                            : static_cast<size_t>(it - buffer.begin());
}

bool readHeaders(Socket& socket, Bytes& buffer) {
  while (headerEnd(buffer) == std::string::npos) {
    if (!socket.recvSome(buffer)) return false;
    if (buffer.size() > 16 * 1024 * 1024) return false;
  }
  return true;
}

bool finishBody(Socket& socket, Bytes& buffer, size_t headEnd, size_t needed) {
  if (buffer.size() < headEnd + 4) return false;
  while (buffer.size() - (headEnd + 4) < needed) {
    Bytes chunk;
    if (!socket.recvSome(chunk)) return false;
    buffer.insert(buffer.end(), chunk.begin(), chunk.end());
  }
  return true;
}


std::optional<HttpRequest> readRequest(Socket& socket, int timeoutMs) {
  socket.setTimeouts(timeoutMs);
  Bytes buffer;
  if (!readHeaders(socket, buffer)) return std::nullopt;
  const size_t headEnd = headerEnd(buffer);
  const std::string headText = toString(Bytes(buffer.begin(), buffer.begin() + headEnd));
  std::stringstream head(headText);

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

  const auto transferEncoding =
      request.headers.find("transfer-encoding");
  if (transferEncoding != request.headers.end() &&
      transferEncoding->second.find("chunked") != std::string::npos) {
    // Chunked request: read the whole body into memory for now.
    // True zero-copy streaming from socket to disk is a follow-up.
    Bytes body;
    // Carry over any body bytes already read past the headers.
    Bytes chunkBuf(buffer.begin() + headEnd + 4, buffer.end());
    buffer.clear();
    while (true) {
      // Read chunk size line.
      while (true) {
        const auto crlf = lineEnd(chunkBuf);
        if (crlf != std::string::npos) break;
        if (!socket.recvSome(chunkBuf)) return std::nullopt;
        if (chunkBuf.size() > 16 * 1024 * 1024) return std::nullopt;
      }
      const auto crlf = lineEnd(chunkBuf);
      const std::string sizeStr =
          toString(Bytes(chunkBuf.begin(), chunkBuf.begin() + crlf));
      chunkBuf.erase(chunkBuf.begin(), chunkBuf.begin() + crlf + 2);
      size_t chunkSize = 0;
      try {
        chunkSize = std::stoul(sizeStr, nullptr, 16);
      } catch (...) {
        return std::nullopt;
      }
      if (chunkSize == 0) {
        while (chunkBuf.size() < 2) {
          if (!socket.recvSome(chunkBuf)) return std::nullopt;
        }
        chunkBuf.erase(chunkBuf.begin(), chunkBuf.begin() + 2);
        break;
      }
      while (chunkBuf.size() < chunkSize + 2) {
        if (!socket.recvSome(chunkBuf)) return std::nullopt;
      }
      body.insert(body.end(), chunkBuf.begin(), chunkBuf.begin() + chunkSize);
      chunkBuf.erase(chunkBuf.begin(), chunkBuf.begin() + chunkSize + 2);
    }
    request.body = body;
    request.extra = chunkBuf;
    return request;
  }

  const auto length = contentLength(request.headers);
  if (length) {
    if (*length > kMaxBodyBytes) {
      request.tooLarge = true;
      return request;
    }
    if (!finishBody(socket, buffer, headEnd, *length)) return std::nullopt;
    const size_t bodyBegin = headEnd + 4;
    request.body = Bytes(buffer.begin() + bodyBegin,
                         buffer.begin() + bodyBegin + *length);
    request.extra = Bytes(buffer.begin() + bodyBegin + *length, buffer.end());
  }
  return request;
}

std::optional<HttpResponse> readResponse(Socket& socket, int timeoutMs) {
  socket.setTimeouts(timeoutMs);
  Bytes buffer;
  if (!readHeaders(socket, buffer)) return std::nullopt;
  const size_t headEnd = headerEnd(buffer);
  const std::string headText = toString(Bytes(buffer.begin(), buffer.begin() + headEnd));
  std::stringstream head(headText);

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

  // Discard header bytes from buffer; only body bytes remain.
  const size_t bodyBegin = headEnd + 4;
  if (buffer.size() > bodyBegin) {
    buffer.erase(buffer.begin(), buffer.begin() + bodyBegin);
  } else {
    buffer.clear();
  }

  const auto transferEncoding = response.headers.find("transfer-encoding");
  if (transferEncoding != response.headers.end() &&
      transferEncoding->second.find("chunked") != std::string::npos) {
    // Read chunked body.
    Bytes body;
    while (true) {
      // Read chunk size line (until \r\n).
      while (true) {
        const auto crlf = lineEnd(buffer);
        if (crlf != std::string::npos) break;
        if (!socket.recvSome(buffer)) return std::nullopt;
        if (buffer.size() > 16 * 1024 * 1024) return std::nullopt;
      }
      const auto crlf = lineEnd(buffer);
      const std::string sizeStr =
          toString(Bytes(buffer.begin(), buffer.begin() + crlf));
      buffer.erase(buffer.begin(), buffer.begin() + crlf + 2);
      size_t chunkSize = 0;
      try {
        chunkSize = std::stoul(sizeStr, nullptr, 16);
      } catch (...) {
        return std::nullopt;
      }
      if (chunkSize == 0) {
        // Read trailing CRLF.
        while (buffer.size() < 2) {
          if (!socket.recvSome(buffer)) return std::nullopt;
        }
        buffer.erase(buffer.begin(), buffer.begin() + 2);
        break;
      }
      // Read chunk data + trailing CRLF.
      while (buffer.size() < chunkSize + 2) {
        if (!socket.recvSome(buffer)) return std::nullopt;
      }
      body.insert(body.end(), buffer.begin(), buffer.begin() + chunkSize);
      buffer.erase(buffer.begin(), buffer.begin() + chunkSize + 2);
    }
    response.body = body;
    return response;
  }

  const auto length = contentLength(response.headers);
  if (length) {
    // buffer may already contain some body bytes (carried over from header read).
    while (buffer.size() < *length) {
      Bytes chunk;
      if (!socket.recvSome(chunk)) return std::nullopt;
      buffer.insert(buffer.end(), chunk.begin(), chunk.end());
    }
    response.body = Bytes(buffer.begin(), buffer.begin() + *length);
    return response;
  }
  // No Content-Length and no chunked: read until connection close.
  // buffer may already contain body bytes carried over from header read.
  Bytes body = std::move(buffer);
  Bytes chunk;
  while (true) {
    chunk.clear();
    if (!socket.recvSome(chunk)) break;
    body.insert(body.end(), chunk.begin(), chunk.end());
  }
  response.body = body;
  return response;
}

HttpClient::Result HttpClient::request(const std::string& method, const std::string& host,
                                       uint16_t port, const std::string& path,
                                       const std::map<std::string, std::string>& headers,
                                       const Bytes& body, int timeoutMs) {
  Result result;
  Socket socket;
  if (!socket.connectTo(host, port, timeoutMs)) {
    result.error = "connect failed: " + host + ":" + std::to_string(port);
    return result;
  }
  std::string head = method + " " + path + " HTTP/1.1\r\n";
  head += "Host: " + host + ":" + std::to_string(port) + "\r\n";
  for (const auto& [key, value] : headers) {
    head += key + ": " + value + "\r\n";
  }
  head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  head += "Connection: close\r\n\r\n";
  Bytes raw = toBytes(head);
  raw.insert(raw.end(), body.begin(), body.end());
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

HttpClient::Result HttpClient::requestStream(const std::string& method,
                                             const std::string& host, uint16_t port,
                                             const std::string& path,
                                             const std::map<std::string, std::string>& headers,
                                             ByteReader reader, int timeoutMs) {
  Result result;
  Socket socket;
  if (!socket.connectTo(host, port, timeoutMs)) {
    result.error = "connect failed: " + host + ":" + std::to_string(port);
    return result;
  }
  std::string head = method + " " + path + " HTTP/1.1\r\n";
  head += "Host: " + host + ":" + std::to_string(port) + "\r\n";
  for (const auto& [key, value] : headers) {
    head += key + ": " + value + "\r\n";
  }
  // Streaming request: chunked transfer encoding.
  head += "Transfer-Encoding: chunked\r\n";
  head += "Connection: close\r\n\r\n";
  if (!socket.sendAll(toBytes(head))) {
    result.error = "send failed";
    return result;
  }
  Bytes chunk;
  while (true) {
    chunk.clear();
    const size_t got = reader(chunk, 65536);
    if (got == 0) break;
    std::ostringstream sizeHead;
    sizeHead << std::hex << got << "\r\n";
    if (!socket.sendAll(toBytes(sizeHead.str()))) {
      result.error = "send failed";
      return result;
    }
    if (!socket.sendAll(chunk)) {
      result.error = "send failed";
      return result;
    }
    const std::string crlf = "\r\n";
    if (!socket.sendAll(toBytes(crlf))) {
      result.error = "send failed";
      return result;
    }
  }
  const std::string trailer = "0\r\n\r\n";
  if (!socket.sendAll(toBytes(trailer))) {
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
      const HttpResponse response = handler(*request, socket);
      const Bytes headers = response.serializeHeaders();
      if (!socket.sendAll(headers)) return;
      if (response.bodyStream) {
        Bytes chunk;
        while (response.bodyStream(chunk)) {
          if (!socket.sendAll(chunk)) return;
          chunk.clear();
        }
      } else {
        socket.sendAll(response.body);
      }
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

void sendText(int fd, const std::string& text) { sendFrame(fd, 0x1, toBytes(text)); }

void sendPing(int fd) { sendFrame(fd, 0x9, Bytes{}); }

namespace {

bool recvInto(int fd, Bytes& buffer) {
  std::vector<std::uint8_t> chunk(65536);
  const ssize_t got = ::recv(fd, reinterpret_cast<char*>(chunk.data()), chunk.size(),
                             MSG_NOSIGNAL);
  if (got > 0) {
    buffer.insert(buffer.end(), chunk.begin(), chunk.begin() + got);
    return true;
  }
  if (got < 0 && errno == EINTR) return true;
  return false;
}

}  // namespace

Frame recvFrame(int fd, Bytes& buffer) {
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
    const size_t payloadBegin = header + maskBytes;
    frame.payload.assign(buffer.begin() + payloadBegin,
                         buffer.begin() + payloadBegin + static_cast<size_t>(length));
    if (masked) {
      for (size_t i = 0; i < frame.payload.size(); ++i) {
        frame.payload[i] = static_cast<std::uint8_t>(frame.payload[i] ^
                                                      buffer[header + (i % 4)]);
      }
    }
    buffer.erase(buffer.begin(), buffer.begin() + total);
    if (opcode == 0x9) {
      sendFrame(fd, 0xA, frame.payload);
      continue;
    }
    if (opcode == 0x8) frame.close = true;
    return frame;
  }
}

Frame recvFrame(int fd) {
  Bytes buffer;
  return recvFrame(fd, buffer);
}

}  // namespace ws

}  // namespace fmtrans::net

