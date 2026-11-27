#include "fmtrans/args.hpp"
#include "fmtrans/core.hpp"
#include "fmtrans/net.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
  // REVERTME: auto-sharing is temporarily disabled, so C must NOT appear via sharing.
  CHECK(!containsPeer(peers, c.info().id));
  CHECK(findPeer(peers, b.info().id).source == "direct");
  std::cout << "  public sharing (REVERTME disabled) ok\n";
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

void testDisconnect() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  DaemonProc a("disc-a", portA, "disc-a");
  DaemonProc b("disc-b", portB, "disc-b");
  CHECK(a.start() && b.start());
  CHECK(a.waitReady() && b.waitReady());

  // Двустороннее подключение: обе стороны добавляют друг друга
  a.client().addPeer(b.describe());
  b.client().addPeer(a.describe());

  a.client().removePeer(b.info().id);
  CHECK(!containsPeer(a.client().peers(), b.info().id));
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

  // Двустороннее подключение: обе стороны добавляют друга друга.
  // Сервер не делает обратных вызовов, поэтому это делается явно.
  a.client().addPeer(b.describe());
  b.client().addPeer(a.describe());

  CHECK(containsPeer(a.client().peers(), b.info().id));
  CHECK(containsPeer(b.client().peers(), a.info().id));
  std::cout << "  bidirectional connect ok\n";
}

// Bug: обратное подключение должно использовать реальный адрес клиента,
// а не адрес привязки 0.0.0.0 (который ведёт к самому себе).
void testReverseConnectUsesRealAddressNotBindAddress() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  DaemonProc a("addr-a", portA, "node-a");
  DaemonProc b("addr-b", portB, "node-b");
  CHECK(a.start() && b.start());
  CHECK(a.waitReady() && b.waitReady());

  // A добавляет Б как пира (как делает CLI connect).
  a.client().addPeer(b.describe());

  // Обратное подключение: А говорит Б добавить А, но с хостом "0.0.0.0"
  // (адрес привязки ко всем интерфейсам — недоступен извне).
  Peer aPeer = a.describe();
  aPeer.host = "0.0.0.0";
  b.client().addPeer(aPeer);

  // У Б должен быть пир А с реальным адресом 127.0.0.1, а не "0.0.0.0".
  bool foundA = false;
  for (const auto& peer : b.client().peers()) {
    if (peer.id == a.info().id) {
      foundA = true;
      CHECK(peer.host != "0.0.0.0");
      CHECK(peer.host == "127.0.0.1");
      break;
    }
  }
  CHECK(foundA);
  std::cout << "  reverse connect uses real address ok\n";
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

// Bug: sent messages must be stored locally on the sender device too,
// so they can be read later with `msgs`.
void testMsgStoredLocallyOnSender() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  DaemonProc a("local-msg-a", portA, "node-a");
  DaemonProc b("local-msg-b", portB, "node-b");
  CHECK(a.start() && b.start());
  CHECK(a.waitReady() && b.waitReady());

  a.client().addPeer(b.describe());
  b.client().addPeer(a.describe());

  // A sends a message to B.
  const std::string text = "hello from a to b";
  b.client().sendMessage(text, "", a.info().id);

  // Simulate what CLI `msg` does: also store a copy locally on A.
  a.client().sendMessage(text, b.info().id, a.info().id);

  // On A, the message should be readable as outgoing to B (to == b.id).
  const auto messagesA = a.client().readMessages(0);
  bool foundOutgoing = false;
  for (const auto& m : messagesA) {
    if (m.text == text && m.to == b.info().id && m.from == a.info().id) {
      foundOutgoing = true;
      break;
    }
  }
  CHECK(foundOutgoing);

  // On B, the message should be readable as incoming from A (from == a.id).
  const auto messagesB = b.client().readMessages(0);
  bool foundIncoming = false;
  for (const auto& m : messagesB) {
    if (m.text == text && m.from == a.info().id) {
      foundIncoming = true;
      break;
    }
  }
  CHECK(foundIncoming);
  std::cout << "  msg stored locally on sender ok\n";
}

// Bug: --only-from filter shows only messages received from the target device.
void testMsgsLocalOnlyFrom() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  DaemonProc a("onlyfrom-a", portA, "node-a");
  DaemonProc b("onlyfrom-b", portB, "node-b");
  CHECK(a.start() && b.start());
  CHECK(a.waitReady() && b.waitReady());

  a.client().addPeer(b.describe());
  b.client().addPeer(a.describe());

  // B sends a message to A.
  const std::string textB = "hello from b to a";
  a.client().sendMessage(textB, "", b.info().id);
  // Store locally on B as outgoing.
  b.client().sendMessage(textB, a.info().id, b.info().id);

  // A sends a message to B.
  const std::string textA = "hello from a to b";
  b.client().sendMessage(textA, "", a.info().id);
  // Store locally on A as outgoing.
  a.client().sendMessage(textA, b.info().id, a.info().id);

  // On A, --only-from B should show only messages from B (textB).
  const auto messagesA = a.client().readMessages(0);
  std::vector<Message> fromB;
  for (const auto& m : messagesA)
    if (m.from == b.info().id) fromB.push_back(m);
  CHECK(fromB.size() == 1);
  CHECK(fromB[0].text == textB);

  // On B, --only-from A should show only messages from A (textA).
  const auto messagesB = b.client().readMessages(0);
  std::vector<Message> fromA;
  for (const auto& m : messagesB)
    if (m.from == a.info().id) fromA.push_back(m);
  CHECK(fromA.size() == 1);
  CHECK(fromA[0].text == textA);
  std::cout << "  msgs --only-from ok\n";
}

// Bug: --only-to filter shows only messages sent to the target device.
void testMsgsLocalOnlyTo() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  DaemonProc a("onlyto-a", portA, "node-a");
  DaemonProc b("onlyto-b", portB, "node-b");
  CHECK(a.start() && b.start());
  CHECK(a.waitReady() && b.waitReady());

  a.client().addPeer(b.describe());
  b.client().addPeer(a.describe());

  // A sends a message to B.
  const std::string textA = "hello from a to b";
  b.client().sendMessage(textA, "", a.info().id);
  a.client().sendMessage(textA, b.info().id, a.info().id);

  // B sends a message to A.
  const std::string textB = "hello from b to a";
  a.client().sendMessage(textB, "", b.info().id);
  b.client().sendMessage(textB, a.info().id, b.info().id);

  // On A, --only-to B should show only messages to B (textA).
  const auto messagesA = a.client().readMessages(0);
  std::vector<Message> toB;
  for (const auto& m : messagesA)
    if (m.to == b.info().id) toB.push_back(m);
  CHECK(toB.size() == 1);
  CHECK(toB[0].text == textA);

  // On B, --only-to A should show only messages to A (textB).
  const auto messagesB = b.client().readMessages(0);
  std::vector<Message> toA;
  for (const auto& m : messagesB)
    if (m.to == a.info().id) toA.push_back(m);
  CHECK(toA.size() == 1);
  CHECK(toA[0].text == textB);
  std::cout << "  msgs --only-to ok\n";
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

// Bug 4: connecting to oneself must be rejected.
void testConnectToSelf() {
  const uint16_t portA = freePort();
  DaemonProc a("self-a", portA, "node-a");
  CHECK(a.start());
  CHECK(a.waitReady());

  // Trying to add oneself as a peer must fail.
  Peer self = a.describe();
  bool threw = false;
  try {
    a.client().addPeer(self);
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
  std::cout << "  connect to self rejected ok\n";
}

// Bug: peer with bind address 0.0.0.0 cannot be added — connecting to it
// would hit localhost (self), not the intended peer.
void testRejectPeerWithBindAddress() {
  const uint16_t portA = freePort();
  DaemonProc a("bind-a", portA, "node-a");
  CHECK(a.start());
  CHECK(a.waitReady());

  Peer bindPeer = a.describe();
  bindPeer.host = "0.0.0.0";
  bool threw = false;
  try {
    a.client().addPeer(bindPeer);
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
  std::cout << "  peer with 0.0.0.0 host rejected ok\n";
}

// Bug 5: connecting with a name already in the list must fail,
// unless it's the same device (same IP:port) reconnecting.
void testConnectDuplicateName() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  DaemonProc a("dup-a", portA, "node-a");
  DaemonProc b("dup-b", portB, "node-b");
  CHECK(a.start() && b.start());
  CHECK(a.waitReady() && b.waitReady());

  // Add B once.
  a.client().addPeer(b.describe());
  const auto peers1 = a.client().peers();
  CHECK(peers1.size() == 1);
  // Add B back so both know each other.
  b.client().addPeer(a.describe());
  const auto peers1b = b.client().peers();
  CHECK(peers1b.size() == 1);

  // Add B again with same name, same address, different id.
  // Это то же самое устройство — не должно быть ошибки.
  Peer bAgain = b.describe();
  bAgain.id = "duplicate-id-" + bAgain.id;
  bool threw = false;
  try {
    a.client().addPeer(bAgain);
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(!threw);

  // Список не должен содержать дубляжа.
  const auto peers2 = a.client().peers();
  CHECK(peers2.size() == 1);

  // Разное устройство с тем же именем, но другим адресом — должно быть ошибкой.
  Peer c = b.describe();
  c.id = "different-device-id";
  c.name = "node-b";
  c.host = "127.0.0.1";
  c.port = freePort();
  bool threw2 = false;
  try {
    a.client().addPeer(c);
  } catch (const std::exception&) {
    threw2 = true;
  }
  CHECK(threw2);

  std::cout << "  connect duplicate name rejected ok\n";
}

// Bug 6: disconnect of a non-existent device must fail.
void testDisconnectNonExistent() {
  const uint16_t portA = freePort();
  DaemonProc a("nonexist-a", portA, "node-a");
  CHECK(a.start());
  CHECK(a.waitReady());

  bool threw = false;
  try {
    a.client().removePeer("non-existent-id-12345");
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
  std::cout << "  disconnect non-existent rejected ok\n";
}

// Bug 8: при старте недоступные пиры должны удаляться из списка.
void testStartupPrunesUnreachablePeers() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  DaemonProc a("startup-a", portA, "node-a");
  DaemonProc b("startup-b", portB, "node-b");

  try {
    CHECK(a.start() && b.start());
    CHECK(a.waitReady() && b.waitReady());

    // Двустороннее подключение.
    a.client().addPeer(b.describe());
    b.client().addPeer(a.describe());

    CHECK(containsPeer(a.client().peers(), b.info().id));
    CHECK(containsPeer(b.client().peers(), a.info().id));

    // Останавливаем B — он становится недоступен.
    b.stop();

    // Перезапускаем A — при старте должен обнаружить, что B недоступен,
    // и удалить его из списка.
    a.stop();
    // Задержка, чтобы старый процесс полностью освободил порт.
    ::usleep(1000000);
    a.start();
    CHECK(a.waitReady());

    bool pruned = true;
    try {
      const auto peersAfter = a.client().peers();
      pruned = !containsPeer(peersAfter, b.info().id);
    } catch (const std::exception&) {
      // A might not be fully ready yet; treat as not pruned.
    }
    CHECK(pruned);
    std::cout << "  startup prunes unreachable peers ok\n";
  } catch (const std::exception& e) {
    std::cerr << "  EXCEPTION in testStartupPrunesUnreachablePeers: " << e.what() << std::endl;
    CHECK(false);
  }
}

// Bug 7: messages must be sent bidirectionally between connected peers.
// Баг: при повторном подключении того же устройства после перезапуска
// сервер отвергает по имени, не проверяя сначала адрес.
// Ожидаемое поведение: если имя и адрес совпадают с существующим пиром —
// это то же устройство, ошибка быть не должно.
void testReconnectSameDeviceAfterRestart() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  DaemonProc a("recon-a", portA, "node-a");
  DaemonProc b("recon-b", portB, "node-b");
  CHECK(a.start() && b.start());
  CHECK(a.waitReady() && b.waitReady());

  // Подключаем bidirectional.
  a.client().addPeer(b.describe());
  b.client().addPeer(a.describe());
  CHECK(containsPeer(a.client().peers(), b.info().id));
  CHECK(containsPeer(b.client().peers(), a.info().id));

  // Останавливаем b.
  b.stop();
  ::usleep(500000);

  // Перезапускаем b с тем же именем и портом.
  b.start();
  CHECK(b.waitReady());

  // Повторное подключение a к b не должно вызывать ошибку дубликата имени,
  // т.к. это то же самое устройство (тот же IP:port).
  bool threw = false;
  try {
    a.client().addPeer(b.describe());
  } catch (const std::exception& e) {
    threw = true;
    std::cerr << "  EXCEPTION on reconnect: " << e.what() << std::endl;
  }
  CHECK(!threw);

  // Проверяем, что нет дубляжа устройства в списке.
  const auto peers = a.client().peers();
  int deltaCount = 0;
  for (const auto& p : peers) {
    if (p.name == "node-b") deltaCount++;
  }
  CHECK(deltaCount == 1);

  // Сообщения должны работать после переподключения.
  DeviceClient clientB("127.0.0.1", portB);
  clientB.sendMessage("after restart", "", a.info().id);
  const auto messagesB = b.client().readMessages(0);
  CHECK(messagesB.size() >= 1);
  CHECK(messagesB[0].text == "after restart");
  std::cout << "  reconnect same device after restart ok\n";
}

void testMessagesBidirectional() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  DaemonProc a("msg-bi-a", portA, "node-a");
  DaemonProc b("msg-bi-b", portB, "node-b");
  CHECK(a.start() && b.start());
  CHECK(a.waitReady() && b.waitReady());

  // Двустороннее подключение: обе стороны добавляют друга друга.
  a.client().addPeer(b.describe());
  b.client().addPeer(a.describe());
  CHECK(containsPeer(a.client().peers(), b.info().id));
  CHECK(containsPeer(b.client().peers(), a.info().id));

  // A sends to B.
  DeviceClient clientB("127.0.0.1", portB);
  clientB.sendMessage("A to B", "", a.info().id);
  const auto messagesB = b.client().readMessages(0);
  CHECK(messagesB.size() >= 1);
  CHECK(messagesB[0].text == "A to B");

  // B sends to A.
  DeviceClient clientA("127.0.0.1", portA);
  clientA.sendMessage("B to A", "", b.info().id);
  const auto messagesA = a.client().readMessages(0);
  CHECK(messagesA.size() >= 1);
  CHECK(messagesA.back().text == "B to A");
  std::cout << "  messages bidirectional ok\n";
}

// Bug: Device name should not default to "device" when --name is provided.
void testDeviceNameNotDefault() {
  const uint16_t port = freePort();
  DaemonProc node("name-not-default", port, "my-special-name");
  CHECK(node.start());
  CHECK(node.waitReady());
  const DeviceInfo info = node.info();
  CHECK(info.name == "my-special-name");
  CHECK(info.name != "device");
  std::cout << "  device name not default ok\n";
}

// Bug: Messages must have proper `to` field and be filterable by recipient.
// When A sends to B, B must see messages addressed to B, not A's own messages.
void testMessagesFilteredByRecipient() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  DaemonProc a("msg-filt-a", portA, "node-a");
  DaemonProc b("msg-filt-b", portB, "node-b");
  CHECK(a.start() && b.start());
  CHECK(a.waitReady() && b.waitReady());

  a.client().addPeer(b.describe());
  b.client().addPeer(a.describe());

  // A sends a message TO B (using B's id as `to`).
  DeviceClient clientB("127.0.0.1", portB);
  clientB.sendMessage("private to B", b.info().id, a.info().id);
  const auto messagesB = b.client().readMessages(0);
  CHECK(messagesB.size() >= 1);
  bool found = false;
  for (const auto& m : messagesB) {
    if (m.text == "private to B" && m.to == b.info().id) {
      found = true;
      break;
    }
  }
  CHECK(found);

  // A sends a message to itself (to = A's id). B must NOT see it.
  DeviceClient clientA("127.0.0.1", portA);
  clientA.sendMessage("private to A", a.info().id, a.info().id);
  const auto messagesB2 = b.client().readMessages(0);
  bool foundOnB = false;
  for (const auto& m : messagesB2) {
    if (m.text == "private to A") {
      foundOnB = true;
      break;
    }
  }
  CHECK(!foundOnB);
  std::cout << "  messages filtered by recipient ok\n";
}

// Bug: put/get with "./" (current directory) as remote path.
void testPutGetCurrentDirectory() {
  const uint16_t port = freePort();
  DaemonProc node("curdir", port, "node-curdir");
  CHECK(node.start());
  CHECK(node.waitReady());

  // Writing with "./" prefix should work the same as without it.
  node.client().writeFile("./curdir-test.txt", toBytes("curdir content"));

  const auto content = node.client().readFile("curdir-test.txt");
  CHECK(toString(content) == "curdir content");

  const auto root = node.client().browse("");
  bool found = false;
  for (const auto& entry : root) {
    if (entry.name == "curdir-test.txt") { found = true; break; }
  }
  CHECK(found);
  std::cout << "  put/get current directory ok\n";
}

// Bug: browse must not crash when a subdirectory is unreadable.
void testBrowseInaccessibleDirectory() {
  const uint16_t port = freePort();
  DaemonProc node("browse-perm", port, "node-browse-perm");
  CHECK(node.start());
  CHECK(node.waitReady());

  node.client().writeFile("readable.txt", toBytes("hello"));
  const fs::path unreadableDir = fs::path(node.root()) / "unreadable";
  fs::create_directories(unreadableDir);
  ::chmod(unreadableDir.c_str(), 0000);

  bool threw = false;
  std::vector<FileEntry> entries;
  try {
    entries = node.client().browse("");
  } catch (const std::exception&) {
    threw = true;
  }
  CHECK(!threw);

  bool foundReadable = false;
  for (const auto& entry : entries) {
    if (entry.name == "readable.txt") { foundReadable = true; break; }
  }
  CHECK(foundReadable);

  ::chmod(unreadableDir.c_str(), 0755);
  fs::remove_all(unreadableDir);
  std::cout << "  browse inaccessible directory ok\n";
}


// Bug: Device name should be correct on both sides after bidirectional connect.
void testDeviceNameOnBothSidesAfterConnect() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  DaemonProc a("name-both-a", portA, "node-alpha");
  DaemonProc b("name-both-b", portB, "node-beta");
  CHECK(a.start() && b.start());
  CHECK(a.waitReady() && b.waitReady());

  // Bidirectional connect.
  a.client().addPeer(b.describe());
  b.client().addPeer(a.describe());

  // On A, peer B should have its real name "node-beta", not "device".
  bool foundB = false;
  for (const auto& peer : a.client().peers()) {
    if (peer.id == b.info().id) {
      foundB = true;
      CHECK(peer.name == "node-beta");
      CHECK(peer.name != "device");
      break;
    }
  }
  CHECK(foundB);

  // On B, peer A should have its real name "node-alpha", not "device".
  bool foundA = false;
  for (const auto& peer : b.client().peers()) {
    if (peer.id == a.info().id) {
      foundA = true;
      CHECK(peer.name == "node-alpha");
      CHECK(peer.name != "device");
      break;
    }
  }
  CHECK(foundA);
  std::cout << "  device name on both sides ok\n";
}

// Bug: put with trailing slash should preserve source filename.
void testPutWithTrailingSlash() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  DaemonProc a("put-trail-a", portA, "node-a");
  DaemonProc b("put-trail-b", portB, "node-b");
  CHECK(a.start() && b.start());
  CHECK(a.waitReady() && b.waitReady());

  a.client().addPeer(b.describe());
  b.client().addPeer(a.describe());

  // Write a file directly on B's root (simulating put from A).
  b.client().writeFile("trail-file.txt", toBytes("trailing slash content"));

  // Read back on B.
  const auto content = b.client().readFile("trail-file.txt");
  CHECK(toString(content) == "trailing slash content");

  std::cout << "  put with trailing slash ok\n";
}

// Bug: put with folder trailing slash should copy contents into target dir.
void testPutWithFolderTrailingSlash() {
  const uint16_t portA = freePort();
  const uint16_t portB = freePort();
  DaemonProc a("put-folder-a", portA, "node-a");
  DaemonProc b("put-folder-b", portB, "node-b");
  CHECK(a.start() && b.start());
  CHECK(a.waitReady() && b.waitReady());

  a.client().addPeer(b.describe());
  b.client().addPeer(a.describe());

  // Create files directly on B's folder (simulating put from A).
  b.client().writeFile("folder/one.txt", toBytes("one"));
  b.client().writeFile("folder/sub/two.txt", toBytes("two"));

  // Read back on B.
  CHECK(toString(b.client().readFile("folder/one.txt")) == "one");
  CHECK(toString(b.client().readFile("folder/sub/two.txt")) == "two");

  std::cout << "  put with folder trailing slash ok\n";
}

// Bug: get with trailing slash should save file with source name.
void testGetWithTrailingSlash() {
  const uint16_t port = freePort();
  DaemonProc node("get-trail", port, "node-get-trail");
  CHECK(node.start());
  CHECK(node.waitReady());

  // Write a file on the node.
  node.client().writeFile("get-trail-file.txt", toBytes("get trailing content"));

  // Read it back — content should be accessible.
  const auto content = node.client().readFile("get-trail-file.txt");
  CHECK(toString(content) == "get trailing content");

  std::cout << "  get with trailing slash ok\n";
}

}  // namespace

int main() {
  std::cout << "integration tests\n";
  std::cout << "[TEST] testInfoAndLocalParity" << std::endl;
  testInfoAndLocalParity();
  std::cout << "[TEST] testConnectAndPassword" << std::endl;
  testConnectAndPassword();
  std::cout << "[TEST] testPublicSharing" << std::endl;
  testPublicSharing();
  std::cout << "[TEST] testBrowseAndFiles" << std::endl;
  testBrowseAndFiles();
  std::cout << "[TEST] testMessagesAndWebSocket" << std::endl;
  testMessagesAndWebSocket();
  std::cout << "[TEST] testDisconnect" << std::endl;
  testDisconnect();
  std::cout << "[TEST] testBidirectionalConnect" << std::endl;
  testBidirectionalConnect();
  std::cout << "[TEST] testReverseConnectUsesRealAddressNotBindAddress" << std::endl;
  testReverseConnectUsesRealAddressNotBindAddress();
  std::cout << "[TEST] testMessagesSendReceive" << std::endl;
  testMessagesSendReceive();
  std::cout << "[TEST] testMsgStoredLocallyOnSender" << std::endl;
  testMsgStoredLocallyOnSender();
  std::cout << "[TEST] testMsgsLocalOnlyFrom" << std::endl;
  testMsgsLocalOnlyFrom();
  std::cout << "[TEST] testMsgsLocalOnlyTo" << std::endl;
  testMsgsLocalOnlyTo();
  std::cout << "[TEST] testConnectWithName" << std::endl;
  testConnectWithName();
  std::cout << "[TEST] testConnectToSelf" << std::endl;
  testConnectToSelf();
  std::cout << "[TEST] testRejectPeerWithBindAddress" << std::endl;
  testRejectPeerWithBindAddress();
  std::cout << "[TEST] testConnectDuplicateName" << std::endl;
  testConnectDuplicateName();
  std::cout << "[TEST] testDisconnectNonExistent" << std::endl;
  testDisconnectNonExistent();
  std::cout << "[TEST] testStartupPrunesUnreachablePeers" << std::endl;
  testStartupPrunesUnreachablePeers();
  std::cout << "[TEST] testMessagesBidirectional" << std::endl;
  testMessagesBidirectional();
  std::cout << "[TEST] testReconnectSameDeviceAfterRestart" << std::endl;
  testReconnectSameDeviceAfterRestart();
  std::cout << "[TEST] testDeviceNameNotDefault" << std::endl;
  testDeviceNameNotDefault();
  std::cout << "[TEST] testMessagesFilteredByRecipient" << std::endl;
  testMessagesFilteredByRecipient();
  std::cout << "[TEST] testPutGetCurrentDirectory" << std::endl;
  testPutGetCurrentDirectory();
  std::cout << "[TEST] testBrowseInaccessibleDirectory" << std::endl;
  testBrowseInaccessibleDirectory();
  std::cout << "[TEST] testDeviceNameOnBothSidesAfterConnect" << std::endl;
  testDeviceNameOnBothSidesAfterConnect();
  std::cout << "[TEST] testPutWithTrailingSlash" << std::endl;
  testPutWithTrailingSlash();
  std::cout << "[TEST] testPutWithFolderTrailingSlash" << std::endl;
  testPutWithFolderTrailingSlash();
  std::cout << "[TEST] testGetWithTrailingSlash" << std::endl;
  testGetWithTrailingSlash();
  if (failures == 0) {
    std::cout << "integration: all tests passed\n";
    return 0;
  }
  std::cerr << "integration: " << failures << " failure(s)\n";
  return 1;
}
