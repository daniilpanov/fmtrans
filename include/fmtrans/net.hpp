#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fmtrans {

// The single payload type used on the wire and for file content.
using Bytes = std::vector<std::uint8_t>;

inline Bytes toBytes(const std::string& text) {
  return Bytes(text.begin(), text.end());
}

// Only for text payloads (JSON, HTTP headers); never for binary file data.
inline std::string toString(const Bytes& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

// Streaming source: fills `out` with up to `maxBytes`, returns bytes read (0 = EOF).
using ByteReader = std::function<size_t(Bytes& out, size_t maxBytes)>;

// Streaming sink: writes `data`, returns false on error.
using ByteWriter = std::function<bool(const Bytes& data)>;

// Streaming producer for a response: fills `chunk` with the next block of data
// and returns true, or returns false when the stream is exhausted/failed.
using BodyProducer = std::function<bool(Bytes& chunk)>;

namespace fs_byte_reader {
// Reads from a std::istream (binary).
ByteReader fromStream(std::istream& input);
}  // namespace fs_byte_reader

}  // namespace fmtrans

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
  bool sendAll(const Bytes& data);
  bool sendAll(const std::string& data);
  bool recvSome(Bytes& out, size_t maxBytes = 65536);

 private:
  int fd_ = -1;
};

struct HttpRequest {
  std::string method;
  std::string path;
  std::string version;
  std::map<std::string, std::string> headers;
  std::map<std::string, std::string> query;
  Bytes body;
  // Optional streaming source for the request body. When set, the server reads
  // the body from this reader instead of from `body`. Used for large uploads.
  ByteReader bodyReader;
  // Bytes that arrived after the request body (e.g. an early WebSocket frame).
  Bytes extra;
  // Client address, used to keep sensitive fields for loopback callers only.
  std::string peer;
  bool tooLarge = false;

  std::string header(const std::string& key) const;
};

struct HttpResponse {
  int status = 200;
  std::string reason;
  std::map<std::string, std::string> headers;
  Bytes body;
  // Optional streaming producer for the response body. When set, the server
  // sends headers (without Content-Length) and then repeatedly calls this
  // producer to get chunks, sending each to the client. Used for large
  // downloads; `body` is ignored.
  BodyProducer bodyStream;

  static HttpResponse json(int status, const std::string& body);
  static HttpResponse text(int status, const std::string& body);
  static HttpResponse bytes(int status, const Bytes& body,
                            const std::string& mime = "application/octet-stream");
  // Serializes headers only (no body). When bodyStream is set, omits
  // Content-Length (uses Connection: close).
  Bytes serializeHeaders() const;
  Bytes serialize() const;
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
                 const Bytes& body = {}, int timeoutMs = 5000);
  // Streaming upload: reads the request body from `reader` in chunks.
  Result requestStream(const std::string& method, const std::string& host,
                       uint16_t port, const std::string& path,
                       const std::map<std::string, std::string>& headers = {},
                       ByteReader reader = {}, int timeoutMs = 5000);
};

class HttpServer {
 public:
  using Handler = std::function<HttpResponse(const HttpRequest&, Socket&)>;
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
  Bytes payload;
  bool close = false;
};

// Reads one frame. The buffer keeps bytes that belong to the next frame.
Frame recvFrame(int fd, Bytes& buffer);
Frame recvFrame(int fd);

}  // namespace ws

// Parses "a=1&b=2" into a map; percent-decoding is applied.
std::map<std::string, std::string> parseQuery(const std::string& raw);

std::string urlDecode(const std::string& value);

// Position of the first "\r\n\r\n" (end of HTTP headers) or npos.
size_t headerEnd(const Bytes& buffer);
// Position of the first "\r\n" (end of a line) or npos.
size_t lineEnd(const Bytes& buffer);

}  // namespace fmtrans::net
