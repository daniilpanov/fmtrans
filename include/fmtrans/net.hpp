#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace fmtrans::net {

class Socket {
 public:
  Socket() = default;
  explicit Socket(int fd) : fd_(fd) {}
  ~Socket();
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;
  Socket(Socket&& other) noexcept;
  Socket& operator=(Socket&& other) noexcept;

  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }
  void close();
  int release();
  bool connectTo(const std::string& host, uint16_t port, int timeoutMs);
  bool setTimeouts(int timeoutMs);
  bool sendAll(const std::string& data);
  bool recvSome(std::string& out, size_t maxBytes = 65536);

 private:
  int fd_ = -1;
};

struct HttpRequest {
  std::string method;
  std::string path;
  std::string version;
  std::map<std::string, std::string> headers;
  std::map<std::string, std::string> query;
  std::string body;
  // Bytes that arrived after the request body (e.g. an early WebSocket frame).
  std::string extra;
  // Client address, used to keep sensitive fields for loopback callers only.
  std::string peer;
  bool tooLarge = false;

  std::string header(const std::string& key) const;
};

struct HttpResponse {
  int status = 200;
  std::string reason;
  std::map<std::string, std::string> headers;
  std::string body;

  static HttpResponse json(int status, const std::string& body);
  static HttpResponse text(int status, const std::string& body);
  static HttpResponse bytes(int status, const std::string& body,
                            const std::string& mime = "application/octet-stream");
  std::string serialize() const;
};

// Reads one full request (headers plus Content-Length body) from a socket.
std::optional<HttpRequest> readRequest(Socket& socket, int timeoutMs = 5000);

// Reads one full response from a socket.
std::optional<HttpResponse> readResponse(Socket& socket, int timeoutMs = 5000);

class HttpClient {
 public:
  struct Result {
    bool ok = false;
    HttpResponse response;
    std::string error;
  };

  Result request(const std::string& method, const std::string& host, uint16_t port,
                 const std::string& path,
                 const std::map<std::string, std::string>& headers = {},
                 const std::string& body = "", int timeoutMs = 5000);
};

class HttpServer {
 public:
  using Handler = std::function<HttpResponse(const HttpRequest&)>;
  using WsHandler = std::function<void(int fd, const HttpRequest&)>;

  HttpServer(uint16_t port, Handler handler, WsHandler wsHandler = {},
             std::string host = "0.0.0.0");

  // Creates and binds the listening socket.
  bool bind();
  // Accept loop; blocks until stop().
  bool serve();
  bool start();
  void stop();
  uint16_t port() const { return port_; }

 private:
  uint16_t port_;
  std::string host_;
  Handler handler_;
  WsHandler wsHandler_;
  int listenFd_ = -1;
  bool stop_ = false;
};

namespace ws {

std::string acceptKey(const std::string& clientKey);

void sendText(int fd, const std::string& text);
void sendPing(int fd);

struct Frame {
  int opcode = -1;
  std::string payload;
  bool close = false;
};

// Reads one frame. The buffer keeps bytes that belong to the next frame.
Frame recvFrame(int fd, std::string& buffer);
Frame recvFrame(int fd);

}  // namespace ws

// Parses "a=1&b=2" into a map; percent-decoding is applied.
std::map<std::string, std::string> parseQuery(const std::string& raw);

std::string urlDecode(const std::string& value);

}  // namespace fmtrans::net
