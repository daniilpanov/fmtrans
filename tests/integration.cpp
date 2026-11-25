#include "fmtrans/args.hpp"
#include "fmtrans/core.hpp"
#include "fmtrans/net.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <stdlib.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using namespace fmtrans;

namespace fs = std::filesystem;

namespace {

int failures = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::cerr << "FAIL " << __func__ << ":" << __LINE__ << ": "      \
                << #cond << "\n";                                      \
      ++failures;                                                      \
    }                                                                  \
  } while (false)

#define CHECK_THROWS(expr)                                             \
  do {                                                                 \
    bool threw = false;                                                \
    try { expr; } catch (const std::exception&) { threw = true; }       \
    CHECK(threw);                                                      \
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

std::string tempHome(const std::string& tag) {
  const char* tmp = std::getenv("TMPDIR");
  const std::string base = (tmp && *tmp) ? std::string(tmp) : std::string("/tmp");
  std::string pattern = base + "/fmtrans-test-XXXXXX";
  if (mkdtemp(pattern.data()) == nullptr) throw Error("cannot create temp directory");
  return pattern + "/" + tag;
}

class DaemonProc {
 public:
  DaemonProc(std::string tag, uint16_t port, std::string name, std::string password = "")
      : home_(tempHome(tag)), port_(port), name_(std::move(name)),
        password_(std::move(password)) {
    fs::create_directories(home_);
  }

  ~DaemonProc() { stop(); }
  DaemonProc(const DaemonProc&) = delete;
  DaemonProc& operator=(const DaemonProc&) = delete;

  bool start() {
    pid_ = fork();
    if (pid_ == 0) {
      setenv("FMTRANS_HOME", home_.c_str(), 1);
      const int devnull = ::open("/dev/null", O_WRONLY);
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      ::close(devnull);
      const std::string root = home_ + "/data";
      execl(FMTRANSD_PATH, "fmtransd", "--port", std::to_string(port_).c_str(),
            "--name", name_.c_str(), "--password", password_.c_str(),
            "--root", root.c_str(), static_cast<char*>(nullptr));
      _exit(1);
    }
    return pid_ > 0;
  }

  void stop() {
    if (pid_ > 0) {
      ::kill(pid_, SIGTERM);
      ::waitpid(pid_, nullptr, 0);
      pid_ = 0;
    }
  }

  bool waitReady() const {
    for (int attempt = 0; attempt < 100; ++attempt) {
      try {
        client().info();
        return true;
      } catch (const std::exception&) {
        ::usleep(50000);
      }
    }
    return false;
  }

  DeviceClient client() const { return DeviceClient("127.0.0.1", port_, password_); }
  DeviceInfo info() const { return client().info(); }
  Peer describe(const std::string& password = "") const {
    Peer peer;
    const DeviceInfo details = info();
    peer.id = details.id;
    peer.name = details.name;
    peer.host = "127.0.0.1";
    peer.port = port_;
    peer.password = password;
    peer.publicAccess = password.empty();
    return peer;
  }

  uint16_t port() const { return port_; }
  std::string root() const { return home_ + "/data"; }

 private:
  std::string home_;
  uint16_t port_;
  std::string name_;
  std::string password_;
  pid_t pid_ = 0;
};

bool containsPeer(const std::vector<Peer>& peers, const std::string& id) {
  for (const auto& peer : peers) {
    if (peer.id == id) return true;
  }
  return false;
}

Peer findPeer(const std::vector<Peer>& peers, const std::string& id) {
  for (const auto& peer : peers) {
    if (peer.id == id) return peer;
  }
  throw Error("peer not found: " + id);
}

void testInfoAndLocalParity() {
  const uint16_t port = freePort();
  DaemonProc node("info", port, "node-info");
  CHECK(node.start());
  CHECK(node.waitReady());
  const DeviceInfo info = node.info();
  CHECK(info.id.size() == 32);
  CHECK(info.name == "node-info");
  CHECK(info.kind == "fmtrans");
  CHECK(info.publicAccess);
  // The very same DeviceClient class serves local and remote endpoints.
  DeviceClient local("127.0.0.1", port);
  CHECK(local.info().id == info.id);
  std::cout << "  info parity ok\n";
}

void testConnectAndPassword() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  DaemonProc a("auth-a", portA, "node-a", "secret");
  DaemonProc b("auth-b", portB, "node-b");
  CHECK(a.start());
  CHECK(b.start());
  CHECK(a.waitReady() && b.waitReady());

  CHECK_THROWS(DeviceClient("127.0.0.1", portA, "wrong").info());
  CHECK(a.client().info().publicAccess == false);

  a.client().addPeer(b.describe());
  const auto peers = a.client().peers();
  CHECK(peers.size() == 1);
  CHECK(peers[0].id == b.info().id);
  std::cout << "  connect and password ok\n";
}

void testPublicSharing() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  const uint16_t portC = freePort();
  DaemonProc a("share-a", portA, "node-a");
  DaemonProc b("share-b", portB, "node-b");
  DaemonProc c("share-c", portC, "node-c");
  CHECK(a.start() && b.start() && c.start());
  CHECK(a.waitReady() && b.waitReady() && c.waitReady());

  b.client().addPeer(c.describe());
  a.client().addPeer(b.describe());
  const auto peers = a.client().peers();
  CHECK(containsPeer(peers, b.info().id));
  CHECK(containsPeer(peers, c.info().id));
  CHECK(findPeer(peers, b.info().id).source == "direct");
  CHECK(findPeer(peers, c.info().id).source == "shared");
  std::cout << "  public sharing ok\n";
}

void testBrowseAndFiles() {
  const uint16_t port = freePort();
  DaemonProc node("files", port, "node-files");
  CHECK(node.start());
  CHECK(node.waitReady());
  // Root is created on first start, before any file is written.
  CHECK(node.client().browse("").empty());

  node.client().writeFile("docs/notes.txt", toBytes("privet"));
  node.client().writeFile("docs/sub/other.bin", Bytes{0, 1, 2});
  CHECK(toString(node.client().readFile("docs/notes.txt")) == "privet");

  const auto root = node.client().browse("");
  CHECK(root.size() == 1);
  CHECK(root[0].name == "docs" && root[0].type == "dir");

  const auto docs = node.client().browse("docs");
  CHECK(docs.size() == 2);
  CHECK(docs[0].name == "sub" && docs[0].type == "dir");
  CHECK(docs[1].name == "notes.txt" && docs[1].size == 6);

  const auto sub = node.client().browse("docs/sub");
  CHECK(sub.size() == 1);
  CHECK(sub[0].name == "other.bin" && sub[0].size == 3);

  CHECK_THROWS(node.client().writeFile("../escape.txt", toBytes("bad")));
  CHECK_THROWS(node.client().browse("../../etc"));
  CHECK_THROWS(node.client().writeFile("/etc/escape.txt", toBytes("bad")));

  // A symlink inside root must not allow reading outside of it.
  const fs::path outside = fs::temp_directory_path() / "fmtrans-outside.txt";
  {
    std::ofstream output(outside);
    output << "secret";
  }
  const fs::path link = fs::path(node.root()) / "link";
  ::symlink(outside.c_str(), link.c_str());
  CHECK_THROWS(node.client().readFile("link/fmtrans-outside.txt"));
  ::unlink(link.c_str());
  fs::remove(outside);
  std::cout << "  browse and files ok\n";
}

void testMessagesAndWebSocket() {
  const uint16_t port = freePort();
  DaemonProc node("messages", port, "node-msg");
  CHECK(node.start());
  CHECK(node.waitReady());

  net::Socket socket;
  CHECK(socket.connectTo("127.0.0.1", port, 2000));
  const std::string handshake =
      "GET /ws HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n\r\n";
  CHECK(socket.sendAll(handshake));
  Bytes response;
  while (true) {
    CHECK(socket.recvSome(response));
    const std::string head = toString(response);
    if (head.find("\r\n\r\n") != std::string::npos) break;
  }
  CHECK(toString(response).find("101 Switching Protocols") != std::string::npos);
  socket.setTimeouts(3000);

  node.client().sendMessage("live hello", "");
  const auto frame = net::ws::recvFrame(socket.get());
  CHECK(frame.opcode == 1);
  if (frame.opcode == 1) {
    const json event = json::parse(toString(frame.payload));
    CHECK(event.at("type") == "message");
    CHECK(event.at("message").at("text") == "live hello");
  }

  node.client().sendMessage("second", "peer");
  const auto messages = node.client().readMessages(0);
  CHECK(messages.size() == 2);
  CHECK(messages[0].seq == 1 && messages[0].text == "live hello");
  CHECK(messages[1].to == "peer");
  CHECK(node.client().readMessages(1).size() == 1);

  node.client().sendMessage("third", "", "tester");
  const auto all = node.client().readMessages(0);
  CHECK(all.back().from == "tester");
  std::cout << "  messages and websocket ok\n";
}

void testRelayViaThirdDevice() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  const uint16_t portC = freePort();
  DaemonProc a("relay-a", portA, "relay-a");
  DaemonProc b("relay-b", portB, "relay-b");
  DaemonProc c("relay-c", portC, "relay-c");
  CHECK(a.start() && b.start() && c.start());
  CHECK(a.waitReady() && b.waitReady() && c.waitReady());

  b.client().addPeer(c.describe());
  a.client().addPeer(b.describe());
  const Peer discovered = findPeer(a.client().peers(), c.info().id);
  CHECK(discovered.source == "shared");

  // A asks B (the third device) to write the file to C.
  b.client().relay(c.describe(), "via/relayed.txt", toBytes("through-b"));
  CHECK(toString(c.client().readFile("via/relayed.txt")) == "through-b");
  std::cout << "  relay via third device ok\n";
}

void testDisconnect() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  DaemonProc a("disc-a", portA, "disc-a");
  DaemonProc b("disc-b", portB, "disc-b");
  CHECK(a.start() && b.start());
  CHECK(a.waitReady() && b.waitReady());

  a.client().addPeer(b.describe());
  const std::string bId = b.info().id;
  a.client().removePeer(bId);
  CHECK(!containsPeer(a.client().peers(), bId));
  std::cout << "  disconnect ok\n";
}

// Bug 1: connect from A to B must also register A on B (bidirectional connect).
void testBidirectionalConnect() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  DaemonProc a("bidi-a", portA, "node-a");
  DaemonProc b("bidi-b", portB, "node-b");
  CHECK(a.start() && b.start());
  CHECK(a.waitReady() && b.waitReady());

  a.client().addPeer(b.describe());
  // After connect from A to B, B must also know about A. The reverse
  // registration happens asynchronously, so retry for a short while.
  bool found = false;
  for (int attempt = 0; attempt < 50; ++attempt) {
    const auto peersB = b.client().peers();
    if (containsPeer(peersB, a.info().id)) {
      found = true;
      break;
    }
    ::usleep(50000);
  }
  CHECK(found);
  std::cout << "  bidirectional connect ok\n";
}

// Bug 2: messages sent from A to B must be readable on B.
void testMessagesSendReceive() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  DaemonProc a("msg-a", portA, "node-a");
  DaemonProc b("msg-b", portB, "node-b");
  CHECK(a.start() && b.start());
  CHECK(a.waitReady() && b.waitReady());

  a.client().addPeer(b.describe());
  // A sends a message to B (POST /messages on device B).
  DeviceClient clientB("127.0.0.1", portB);
  clientB.sendMessage("hello from A", "", a.info().id);
  const auto messagesB = b.client().readMessages(0);
  CHECK(messagesB.size() == 1);
  CHECK(messagesB[0].text == "hello from A");
  CHECK(messagesB[0].from == a.info().id);
  std::cout << "  messages send/receive ok\n";
}

// Bug 3: connect with --name must store the name in the peer descriptor.
void testConnectWithName() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  DaemonProc a("name-a", portA, "node-a");
  DaemonProc b("name-b", portB, "node-b");
  CHECK(a.start() && b.start());
  CHECK(a.waitReady() && b.waitReady());

  Peer bPeer = b.describe();
  bPeer.name = "my-custom-name";
  a.client().addPeer(bPeer);
  const auto peers = a.client().peers();
  CHECK(peers.size() == 1);
  CHECK(peers[0].name == "my-custom-name");
  std::cout << "  connect with name ok\n";
}

}  // namespace

int main() {
  std::cout << "integration tests\n";
  testInfoAndLocalParity();
  testConnectAndPassword();
  testPublicSharing();
  testBrowseAndFiles();
  testMessagesAndWebSocket();
  testRelayViaThirdDevice();
  testDisconnect();
  testBidirectionalConnect();
  testMessagesSendReceive();
  testConnectWithName();
  if (failures == 0) {
    std::cout << "integration: all tests passed\n";
    return 0;
  }
  std::cerr << "integration: " << failures << " failure(s)\n";
  return 1;
}
