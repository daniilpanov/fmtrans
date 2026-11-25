#include "fmtrans/core.hpp"

#include "fmtrans/platform.hpp"

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace fmtrans {
namespace {

std::string encode(const std::string& value) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else if (c == ' ') {
      out.push_back('%');
      out.push_back('2');
      out.push_back('0');
    } else {
      out.push_back('%');
      out.push_back(hex[c >> 4]);
      out.push_back(hex[c & 0x0f]);
    }
  }
  return out;
}

}  // namespace

json DeviceInfo::toJson() const {
  return {{"id", id}, {"name", name}, {"kind", kind},
          {"version", version}, {"public", publicAccess}};
}

DeviceInfo DeviceInfo::fromJson(const json& value) {
  DeviceInfo info;
  info.id = value.value("id", "");
  info.name = value.value("name", "");
  info.kind = value.value("kind", "");
  info.version = value.value("version", 0);
  info.publicAccess = value.value("public", false);
  return info;
}

json Peer::toJson(bool maskPassword) const {
  return {{"id", id},
          {"name", name},
          {"host", host},
          {"port", port},
          {"password", maskPassword && !password.empty() ? "***" : password},
          {"public", publicAccess},
          {"source", source}};
}

Peer Peer::fromJson(const json& value) {
  Peer peer;
  peer.id = value.value("id", "");
  peer.name = value.value("name", "");
  peer.host = value.value("host", "");
  peer.port = static_cast<uint16_t>(value.value("port", 0));
  peer.password = value.value("password", "");
  peer.publicAccess = value.value("public", peer.password.empty());
  peer.source = value.value("source", "direct");
  return peer;
}

json FileEntry::toJson() const {
  return {{"name", name}, {"type", type}, {"size", size}, {"mtime", mtime}};
}

FileEntry FileEntry::fromJson(const json& value) {
  FileEntry entry;
  entry.name = value.value("name", "");
  entry.type = value.value("type", "file");
  entry.size = value.value("size", static_cast<uint64_t>(0));
  entry.mtime = value.value("mtime", "");
  return entry;
}

json Message::toJson() const {
  return {{"seq", seq}, {"from", from}, {"to", to}, {"text", text}, {"ts", ts}};
}

Message Message::fromJson(const json& value) {
  Message message;
  message.seq = value.value("seq", static_cast<uint64_t>(0));
  message.from = value.value("from", "");
  message.to = value.value("to", "");
  message.text = value.value("text", "");
  message.ts = value.value("ts", "");
  return message;
}

std::string homeDir() {
  const char* env = std::getenv("FMTRANS_HOME");
  std::string path = env ? env : std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") + "/.fmtrans";
  std::filesystem::create_directories(path);
  platform::setFilePermissions(path, 0700);
  return path;
}

LocalConfig loadLocalConfig() {
  LocalConfig config;
  config.root = homeDir() + "/data";
  const std::string path = homeDir() + "/local.conf";
  std::ifstream input(path);
  if (!input) return config;
  try {
    json value;
    input >> value;
    config.host = value.value("host", config.host);
    config.port = static_cast<uint16_t>(value.value("port", config.port));
    config.name = value.value("name", config.name);
    config.password = value.value("password", config.password);
    config.root = value.value("root", config.root);
  } catch (const std::exception&) {
    // Broken config: fall back to defaults.
  }
  return config;
}

void saveLocalConfig(const LocalConfig& config) {
  const json value = {{"host", config.host}, {"port", config.port},
                      {"name", config.name}, {"password", config.password},
                      {"root", config.root}};
  const std::string path = homeDir() + "/local.conf";
  const std::string temporary = path + ".tmp";
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    output << value.dump(2);
  }
  platform::setFilePermissions(temporary, 0600);
  std::filesystem::rename(temporary, path);
}

std::string makeId() {
  std::random_device device;
  std::mt19937_64 generator(static_cast<uint64_t>(device()) << 32 | device());
  std::ostringstream out;
  out << std::hex << std::setfill('0') << std::setw(16) << generator()
      << std::setw(16) << generator();
  return out.str();
}

std::string nowIso() {
  const auto now = std::chrono::system_clock::now();
  std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&time, &tm);
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &tm);
  return buffer;
}

DeviceClient::DeviceClient(std::string host, uint16_t port, std::string password)
    : host_(std::move(host)), port_(port), password_(std::move(password)) {}

net::HttpClient::Result DeviceClient::raw(const std::string& method, const std::string& path,
                                          const Bytes& body,
                                          const std::string& contentType) const {
  std::map<std::string, std::string> headers;
  if (!password_.empty()) headers["X-Fmtrans-Auth"] = password_;
  if (!contentType.empty()) headers["Content-Type"] = contentType;
  return net::HttpClient().request(method, host_, port_, "/api/v1" + path, headers, body);
}

json DeviceClient::call(const std::string& method, const std::string& path,
                        const json& body) const {
  const Bytes payload = body.is_null() ? Bytes{} : toBytes(body.dump());
  const auto result = raw(method, path, payload, "application/json");
  if (!result.ok) throw Error("unreachable: " + host_ + ":" + std::to_string(port_));
  json response;
  try {
    response = json::parse(toString(result.response.body));
  } catch (const std::exception&) {
    throw Error("invalid response from " + addressHost());
  }
  if (!response.value("ok", false)) {
    throw Error(response.value("error", "request failed"));
  }
  return response.contains("data") ? response["data"] : json::object();
}

std::string DeviceClient::addressHost() const {
  return host_ + ":" + std::to_string(port_);
}

DeviceInfo DeviceClient::info() const {
  return DeviceInfo::fromJson(call("GET", "/info"));
}

std::vector<Peer> DeviceClient::peers() const {
  std::vector<Peer> result;
  for (const auto& item : call("GET", "/peers")) {
    result.push_back(Peer::fromJson(item));
  }
  return result;
}

void DeviceClient::addPeer(const Peer& peer) { call("POST", "/peers", peer.toJson()); }

void DeviceClient::removePeer(const std::string& id) {
  call("DELETE", "/peers/" + encode(id));
}

std::vector<FileEntry> DeviceClient::browse(const std::string& path) const {
  std::vector<FileEntry> result;
  for (const auto& item : call("GET", "/fs?path=" + encode(path))) {
    result.push_back(FileEntry::fromJson(item));
  }
  return result;
}

Bytes DeviceClient::readFile(const std::string& path) const {
  Bytes result;
  readFileStream(path, [&result](const Bytes& chunk) -> bool {
    result.insert(result.end(), chunk.begin(), chunk.end());
    return true;
  });
  return result;
}

void DeviceClient::readFileStream(const std::string& path,
                                  ByteWriter writer) const {
  const auto result = raw("GET", "/fs/file?path=" + encode(path));
  if (!result.ok) throw Error("read file failed: " + path);
  if (result.response.status != 200) {
    std::string message = "read file failed: HTTP " +
                          std::to_string(result.response.status);
    try {
      const json error = json::parse(toString(result.response.body));
      if (!error.value("ok", true)) message = error.value("error", message);
    } catch (const std::exception&) {
    }
    throw Error(message);
  }
  // Buffered response: stream body chunks to writer.
  const size_t chunkSize = 65536;
  for (size_t off = 0; off < result.response.body.size(); off += chunkSize) {
    Bytes chunk(result.response.body.begin() + off,
                result.response.body.begin() +
                    std::min(off + chunkSize, result.response.body.size()));
    if (!writer(chunk)) throw Error("stream write failed: " + path);
  }
}

void DeviceClient::writeFile(const std::string& path,
                             ByteReader reader) {
  const std::string query = "/fs/file?path=" + encode(path);
  // Use streaming upload via chunked transfer encoding.
  const auto r = rawStream("PUT", query, reader, "application/octet-stream");
  if (!r.ok) throw Error("write file failed: " + path);
  json response;
  try {
    response = json::parse(toString(r.response.body));
  } catch (const std::exception&) {
    throw Error("write file failed: HTTP " +
                std::to_string(r.response.status));
  }
  if (!response.value("ok", false)) throw Error(response.value("error", "write failed"));
}

void DeviceClient::sendMessage(const std::string& text, const std::string& to,
                               const std::string& from) {
  call("POST", "/messages", {{"text", text}, {"to", to}, {"from", from}});
}

std::vector<Message> DeviceClient::readMessages(uint64_t since) const {
  std::vector<Message> result;
  for (const auto& item : call("GET", "/messages?since=" + std::to_string(since))) {
    result.push_back(Message::fromJson(item));
  }
  return result;
}

net::HttpClient::Result DeviceClient::rawStream(
    const std::string& method, const std::string& path,
    ByteReader reader, const std::string& contentType) const {
  std::map<std::string, std::string> headers;
  if (!password_.empty()) headers["X-Fmtrans-Auth"] = password_;
  if (!contentType.empty()) headers["Content-Type"] = contentType;
  return net::HttpClient().requestStream(method, host_, port_, "/api/v1" + path,
                                         headers, reader);
}

}  // namespace fmtrans
