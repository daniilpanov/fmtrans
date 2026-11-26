#include "fmtrans/state.hpp"

#include "fmtrans/platform.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fmtrans {
namespace fs = std::filesystem;

NodeState::NodeState(LocalConfig config) : config_(std::move(config)) {
  info_.id = makeId();
  info_.name = config_.name;
  info_.publicAccess = config_.password.empty();
}

void NodeState::load() {
  fs::create_directories(config_.root);
  const std::string path = homeDir() + "/state.json";
  std::ifstream input(path);
  if (!input) {
    save();
    return;
  }
  try {
    json value;
    input >> value;
    info_.id = value.value("id", info_.id);
    info_.name = value.value("name", info_.name);
    info_.publicAccess = config_.password.empty();
    nextSeq_ = value.value("nextSeq", static_cast<uint64_t>(1));
    for (const auto& item : value.value("peers", json::array())) {
      peers_.push_back(Peer::fromJson(item));
    }
    for (const auto& item : value.value("messages", json::array())) {
      messages_.push_back(Message::fromJson(item));
    }
  } catch (const std::exception&) {
    // Corrupted state starts clean but keeps the generated identity.
  }
}

void NodeState::save() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  json peers = json::array();
  for (const auto& peer : peers_) peers.push_back(peer.toJson());
  json messages = json::array();
  for (const auto& message : messages_) messages.push_back(message.toJson());
  const json value = {{"id", info_.id}, {"name", info_.name},
                      {"peers", peers}, {"messages", messages},
                      {"nextSeq", nextSeq_}};
  const std::string path = homeDir() + "/state.json";
  const std::string temporary = path + ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    output << value.dump(2);
  }
  platform::setFilePermissions(temporary, 0600);
  fs::rename(temporary, path);
}

bool NodeState::authorized(const std::string& token) const {
  if (config_.password.empty()) return true;
  const std::string& expected = config_.password;
  if (token.size() != expected.size()) return false;
  unsigned char diff = 0;
  for (size_t i = 0; i < expected.size(); ++i) {
    diff |= static_cast<unsigned char>(token[i]) ^ static_cast<unsigned char>(expected[i]);
  }
  return diff == 0;
}

std::vector<Peer> NodeState::peers() const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return peers_;
}

void NodeState::addPeer(const Peer& peer) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  if (callbackInProgress_) return;
  Peer stored = peer;
  if (stored.id.empty()) stored.id = makeId();
  // Cannot add oneself as a peer.
  if (stored.id == info_.id) throw Error("cannot add self as peer");
  // Single source of truth: reject duplicates by id, name, or address.
  for (const auto& item : peers_) {
    // Same ID — то же устройство, уже в списке, ничего не делаем.
    if (item.id == stored.id) return;
    const bool sameAddress = (item.host == stored.host && item.port == stored.port);
    const bool sameName = !stored.name.empty() && item.name == stored.name;
    // Имя и адрес совпадают — это то же устройство после перезапуска,
    // повторное подключение не является ошибкой.
    if (sameAddress && sameName) return;
    // Имя занято другим устройством.
    if (sameName) throw Error("peer with name " + stored.name + " already exists");
    // Адрес занят другим устройством.
    if (sameAddress) throw Error("peer at " + stored.address() + " already exists");
  }
  peers_.push_back(stored);
  save();
}

void NodeState::removePeer(const std::string& id) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  const auto it = std::find_if(peers_.begin(), peers_.end(),
                               [&](const Peer& peer) {
                                 return peer.id == id || peer.name == id ||
                                        peer.address() == id;
                               });
  if (it == peers_.end()) return;
  peers_.erase(it);
  save();
}

std::optional<Peer> NodeState::findPeer(const std::string& key) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (const auto& peer : peers_) {
    if (peer.id == key || peer.name == key || peer.address() == key) return peer;
  }
  return std::nullopt;
}

std::optional<Peer> NodeState::findPeerByName(const std::string& name) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  for (const auto& peer : peers_) {
    if (peer.name == name) return peer;
  }
  return std::nullopt;
}

std::vector<Message> NodeState::messagesSince(uint64_t since) const {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::vector<Message> result;
  const std::string selfId = info_.id;
  for (const auto& message : messages_) {
    if (message.seq > since &&
        (message.to.empty() || message.to == selfId || message.from == selfId)) {
      result.push_back(message);
    }
  }
  return result;
}

uint64_t NodeState::addMessage(const std::string& from, const std::string& to,
                               const std::string& text) {
  Message message;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    message.seq = nextSeq_++;
    message.from = from;
    message.to = to;
    message.text = text;
    message.ts = nowIso();
    messages_.push_back(message);
    save();
  }
  broadcast({{"type", "message"}, {"message", message.toJson()}});
  return message.seq;
}

fs::path NodeState::resolve(const std::string& relative) const {
  const fs::path root = fs::absolute(config_.root).lexically_normal();
  fs::path target = relative.empty() ? root : fs::path(relative);
  if (!target.is_absolute()) target = root / target;
  target = target.lexically_normal();
  const auto inside = [](const fs::path& path, const fs::path& base) {
    const fs::path child = path.lexically_relative(base);
    return !child.empty() && !child.begin()->empty() && *child.begin() != "..";
  };
  if (!inside(target, root)) throw Error("path outside root: " + relative);
  return target;
}

std::vector<FileEntry> NodeState::browse(const std::string& relative) const {
  const fs::path target = resolve(relative);
  if (!fs::exists(target)) throw Error("not found: " + relative);
  if (!fs::is_directory(target)) throw Error("not a directory: " + relative);

  std::vector<FileEntry> entries;
  for (const auto& item : fs::directory_iterator(target)) {
    FileEntry entry;
    entry.name = item.path().filename().string();
    const bool isDir = item.is_directory();
    entry.type = isDir ? "dir" : "file";
    if (!isDir) entry.size = item.file_size();
    const auto modified = item.last_write_time();
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
                             modified.time_since_epoch())
                             .count();
    std::time_t time = static_cast<std::time_t>(seconds);
    std::tm tm{};
    gmtime_r(&time, &tm);
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
    entry.mtime = buffer;
    entries.push_back(entry);
  }
  std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
    if (a.type != b.type) return a.type == "dir";
    return a.name < b.name;
  });
  return entries;
}

Bytes NodeState::readFile(const std::string& relative) const {
  Bytes result;
  readFileStream(relative, [&result](const Bytes& chunk) -> bool {
    result.insert(result.end(), chunk.begin(), chunk.end());
    return true;
  });
  return result;
}

void NodeState::readFileStream(const std::string& relative,
                               ByteWriter writer) const {
#if defined(_WIN32)
  throw Error("file streaming not implemented on Windows");
#else
  const fs::path target = resolve(relative);
  if (!fs::exists(target) || !fs::is_regular_file(target)) {
    throw Error("not a file: " + relative);
  }
  const int fd = ::open(target.c_str(), O_RDONLY);
  if (fd < 0) throw Error("cannot open file: " + relative);
  Bytes chunk;
  chunk.resize(65536);
  while (true) {
    chunk.resize(65536);
    const ssize_t got = ::read(fd, chunk.data(), 65536);
    if (got < 0) {
      ::close(fd);
      throw Error("read error: " + relative);
    }
    if (got == 0) break;
    chunk.resize(static_cast<size_t>(got));
    if (!writer(chunk)) {
      ::close(fd);
      throw Error("stream write failed: " + relative);
    }
  }
  ::close(fd);
#endif
}

void NodeState::writeFile(const std::string& relative,
                          ByteReader reader) {
#if defined(_WIN32)
  throw Error("file streaming not implemented on Windows");
#else
  const fs::path target = resolve(relative);
  fs::create_directories(target.parent_path());
  const int fd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) throw Error("cannot open file: " + relative);
  Bytes chunk;
  chunk.resize(65536);
  while (true) {
    chunk.clear();
    const size_t got = reader(chunk, 65536);
    if (got == 0) break;
    chunk.resize(got);
    ssize_t written = ::write(fd, chunk.data(), chunk.size());
    if (written < 0 || static_cast<size_t>(written) < chunk.size()) {
      ::close(fd);
      throw Error("write failed: " + relative);
    }
  }
  ::close(fd);
#endif
}

void NodeState::setCallbackInProgress(bool v) {
  callbackInProgress_.store(v);
}

void NodeState::subscribe(int fd) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  subscribers_.push_back(fd);
}

void NodeState::unsubscribe(int fd) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  subscribers_.erase(std::remove(subscribers_.begin(), subscribers_.end(), fd),
                     subscribers_.end());
}

void NodeState::broadcast(const json& event) {
  std::vector<int> targets;
  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    targets = subscribers_;
  }
  const std::string payload = event.dump();
  for (int fd : targets) {
    if (::fcntl(fd, F_GETFD) == -1) continue;
    net::ws::sendText(fd, payload);
  }
}

}  // namespace fmtrans
