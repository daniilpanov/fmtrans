#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "fmtrans/net.hpp"

namespace fmtrans {

using json = nlohmann::json;

class Error : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct DeviceInfo {
  std::string id;
  std::string name;
  std::string kind = "fmtrans";
  int version = 1;
  bool publicAccess = false;

  json toJson() const;
  static DeviceInfo fromJson(const json& value);
};

struct Peer {
  std::string id;
  std::string name;
  std::string host;
  uint16_t port = 0;
  std::string password;
  bool publicAccess = false;
  // "direct" - connected explicitly, "shared" - discovered from another peer.
  std::string source = "direct";

  // maskPassword replaces a non-empty password with "***" for remote callers.
  json toJson(bool maskPassword = false) const;
  static Peer fromJson(const json& value);
  std::string address() const { return host + ":" + std::to_string(port); }
};

struct FileEntry {
  std::string name;
  std::string type = "file";  // "file" or "dir"
  uint64_t size = 0;
  std::string mtime;

  json toJson() const;
  static FileEntry fromJson(const json& value);
};

struct Message {
  uint64_t seq = 0;
  std::string from;
  std::string to;
  std::string text;
  std::string ts;

  json toJson() const;
  static Message fromJson(const json& value);
};

// The single interface used for every device: local daemon and remote peers alike.
class IDevice {
 public:
  virtual ~IDevice() = default;
  virtual DeviceInfo info() const = 0;
  virtual std::vector<Peer> peers() const = 0;
  virtual void addPeer(const Peer& peer) = 0;
  virtual void removePeer(const std::string& id) = 0;
  virtual std::vector<FileEntry> browse(const std::string& path) const = 0;
  virtual std::string readFile(const std::string& path) const = 0;
  virtual void writeFile(const std::string& path, const std::string& data) = 0;
  virtual void sendMessage(const std::string& text, const std::string& to,
                           const std::string& from = "") = 0;
  virtual std::vector<Message> readMessages(uint64_t since) const = 0;
  virtual void relay(const Peer& target, const std::string& path,
                     const std::string& data) = 0;
};

class DeviceClient : public IDevice {
 public:
  DeviceClient(std::string host, uint16_t port, std::string password = "");

  DeviceInfo info() const override;
  std::vector<Peer> peers() const override;
  void addPeer(const Peer& peer) override;
  void removePeer(const std::string& id) override;
  std::vector<FileEntry> browse(const std::string& path) const override;
  std::string readFile(const std::string& path) const override;
  void writeFile(const std::string& path, const std::string& data) override;
  void sendMessage(const std::string& text, const std::string& to,
                   const std::string& from = "") override;
  std::vector<Message> readMessages(uint64_t since) const override;
  void relay(const Peer& target, const std::string& path,
             const std::string& data) override;

  const std::string& host() const { return host_; }
  uint16_t port() const { return port_; }
  const std::string& password() const { return password_; }

 private:
  net::HttpClient::Result raw(const std::string& method, const std::string& path,
                              const std::string& body = "",
                              const std::string& contentType = "") const;
  json call(const std::string& method, const std::string& path,
            const json& body = json::object()) const;
  std::string addressHost() const;

  std::string host_;
  uint16_t port_;
  std::string password_;
};

struct LocalConfig {
  std::string host = "127.0.0.1";
  uint16_t port = 9000;
  std::string name = "device";
  std::string password;
  std::string root = "data";
};

std::string homeDir();
LocalConfig loadLocalConfig();
void saveLocalConfig(const LocalConfig& config);
std::string makeId();
std::string nowIso();

}  // namespace fmtrans
