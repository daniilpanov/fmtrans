#include "fmtrans/core.hpp"
#include "fmtrans/net.hpp"
#include "fmtrans/sha1.hpp"

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <iostream>
#include <thread>

using namespace fmtrans;

namespace {

int failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::cerr << "FAIL line " << __LINE__ << ": " << #cond << "\n";     \
      ++failures;                                                         \
    }                                                                     \
  } while (false)

uint16_t freePort() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  socklen_t len = sizeof(address);
  ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &len);
  const auto port = ntohs(address.sin_port);
  ::close(fd);
  return port;
}

void testSha1() {
  CHECK(sha1Hex("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
  CHECK(sha1Hex("") == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  CHECK(base64Encode("f") == "Zg==");
  CHECK(base64Encode("fo") == "Zm8=");
  CHECK(base64Encode("foo") == "Zm9v");
  CHECK(net::ws::acceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

void testQuery() {
  CHECK(net::urlDecode("a+b%20c") == "a b c");
  CHECK(net::urlDecode("%D1%82%D0%B5%D1%81%D1%82") == "тест");
  const auto query = net::parseQuery("path=/tmp/a%20b&x=1&empty=");
  CHECK(query.at("path") == "/tmp/a b");
  CHECK(query.at("x") == "1");
  CHECK(query.at("empty") == "");
}

void testSerialize() {
  const auto response = net::HttpResponse::json(200, "{\"ok\":true}");
  const std::string raw = toString(response.serialize());
  CHECK(raw.find("HTTP/1.1 200 OK\r\n") == 0);
  CHECK(raw.find("Content-Length: 11\r\n") != std::string::npos);
  CHECK(raw.find("application/json") != std::string::npos);
}

void testHttpRoundtrip() {
  const uint16_t port = freePort();
  net::HttpServer server(
    port, [](const net::HttpRequest& request, net::Socket&) {
      if (request.path == "/echo") {
        return net::HttpResponse::text(200,
                                       request.method + ":" + toString(request.body) +
                                           ":" + request.header("x-test"));
      }
      return net::HttpResponse::json(404, "{\"ok\":false}");
    });
  std::atomic<bool> ready{false};
  std::thread worker([&] {
    ready = true;
    server.start();
  });
  while (!ready) std::this_thread::yield();
  net::HttpClient client;
  const auto result = client.request("POST", "127.0.0.1", port, "/echo",
                                     {{"X-Test", "value"}}, toBytes("payload"));
  CHECK(result.ok);
  CHECK(result.response.status == 200);
  CHECK(toString(result.response.body) == "POST:payload:value");
  const auto missing = client.request("GET", "127.0.0.1", port, "/missing");
  CHECK(missing.ok && missing.response.status == 404);
  server.stop();
  worker.join();
}

void testWebSocketFrames() {
  int pair[2];
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  net::ws::sendText(pair[0], "privet");
  const auto frame = net::ws::recvFrame(pair[1]);
  CHECK(frame.opcode == 0x1);
  CHECK(toString(frame.payload) == "privet");
  CHECK(!frame.close);
  ::close(pair[0]);
  ::close(pair[1]);
}

void testWebSocketBuffered() {
  int pair[2];
  CHECK(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
  net::ws::sendText(pair[0], "one");
  net::ws::sendText(pair[0], "two");
  Bytes buffer;
  const auto first = net::ws::recvFrame(pair[1], buffer);
  CHECK(first.opcode == 0x1 && toString(first.payload) == "one" &&
        !first.close);
  const auto second = net::ws::recvFrame(pair[1], buffer);
  CHECK(second.opcode == 0x1 && toString(second.payload) == "two" &&
        !second.close);
  ::close(pair[0]);
  const auto closed = net::ws::recvFrame(pair[1], buffer);
  CHECK(closed.close);
  ::close(pair[1]);
}

void testPeerMasking() {
  // Password removed: peer now has no password field.
  Peer peer;
  peer.id = "id";
  peer.name = "name";
  peer.host = "host";
  peer.port = 9100;
  peer.publicAccess = true;
  CHECK(peer.toJson().at("public") == true);
}

}  // namespace

int main() {
  testSha1();
  testQuery();
  testSerialize();
  testHttpRoundtrip();
  testWebSocketFrames();
  testWebSocketBuffered();
  testPeerMasking();
  if (failures == 0) {
    std::cout << "unit: all tests passed\n";
    return 0;
  }
  std::cerr << "unit: " << failures << " failure(s)\n";
  return 1;
}
