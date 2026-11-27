#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "fmtrans/core.hpp"

namespace fmtrans {

class NodeState {
 public:
  explicit NodeState(LocalConfig config);

  void load();
  void save() const;

  const DeviceInfo& info() const { return info_; }
  const LocalConfig& config() const { return config_; }

  std::vector<Peer> peers() const;
  void addPeer(const Peer& peer);
  void removePeer(const std::string& id);
  std::optional<Peer> findPeer(const std::string& key) const;
  std::optional<Peer> findPeerByName(const std::string& name) const;

  std::vector<Message> messagesSince(uint64_t since) const;
  uint64_t addMessage(const std::string& from, const std::string& to,
                      const std::string& text);

  std::vector<FileEntry> browse(const std::string& relative) const;
  Bytes readFile(const std::string& relative) const;
  void readFileStream(const std::string& relative,
                      ByteWriter writer) const;
  void writeFile(const std::string& relative, ByteReader reader);

  void subscribe(int fd);
  void unsubscribe(int fd);
  void broadcast(const json& event);

  // Resolves a relative path to an absolute path inside root (safety-checked).
  std::filesystem::path resolve(const std::string& relative) const;

  // True while this node is processing a callback addPeer from a remote node.
  // Prevents infinite recursion when two nodes connect to each other.
  std::atomic<bool> callbackInProgress_ = false;

  void setCallbackInProgress(bool v);

 private:

  LocalConfig config_;
  DeviceInfo info_;
  std::vector<Peer> peers_;
  std::vector<Message> messages_;
  uint64_t nextSeq_ = 1;
  mutable std::recursive_mutex mutex_;
  std::vector<int> subscribers_;
};

}  // namespace fmtrans
