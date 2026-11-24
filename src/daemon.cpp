#include "fmtrans/daemon.hpp"

#include <unistd.h>

#include <algorithm>
#include <iostream>

namespace fmtrans {
namespace {

json success(const json& data = json::object()) {
  return {{"ok", true}, {"data", data}};
}

json entriesToJson(const std::vector<FileEntry>& entries) {
  json result = json::array();
  for (const auto& entry : entries) result.push_back(entry.toJson());
  return result;
}

json messagesToJson(const std::vector<Message>& messages) {
  json result = json::array();
  for (const auto& message : messages) result.push_back(message.toJson());
  return result;
}

// Only the local CLI may see stored peer passwords.
bool loopback(const net::HttpRequest& request) {
  return request.peer == "127.0.0.1" || request.peer == "::1";
}

bool knownPath(const std::string& path) {
  return path == "/api/v1/info" || path == "/api/v1/peers" ||
         path.rfind("/api/v1/peers/", 0) == 0 || path == "/api/v1/fs" ||
         path == "/api/v1/fs/file" || path == "/api/v1/messages" ||
         path == "/api/v1/relay";
}

}  // namespace

Daemon::Daemon(LocalConfig config)
    : state_(config),
      server_(config.port,
              [this](const net::HttpRequest& request) { return handle(request); },
              [this](int fd, const net::HttpRequest& request) { handleWs(fd, request); },
              config.host) {}

net::HttpResponse Daemon::error(int status, const std::string& message) const {
  return net::HttpResponse::json(status, json({{"ok", false}, {"error", message}}).dump());
}

net::HttpResponse Daemon::handle(const net::HttpRequest& request) {
  const std::string& path = request.path;
  const std::string& method = request.method;

  if (!state_.authorized(request.header("x-fmtrans-auth"))) {
    return error(401, "unauthorized");
  }

  try {
    if (method == "GET" && path == "/api/v1/info") {
      return net::HttpResponse::json(200, success(state_.info().toJson()).dump());
    }

    if (method == "GET" && path == "/api/v1/peers") {
      const bool mask = !loopback(request);
      json peers = json::array();
      for (const auto& peer : state_.peers()) peers.push_back(peer.toJson(mask));
      return net::HttpResponse::json(200, success(peers).dump());
    }

    if (method == "POST" && path == "/api/v1/peers") {
      Peer peer = Peer::fromJson(json::parse(toString(request.body)));
      if (peer.host.empty() || peer.port == 0) {
        return error(400, "host and port are required");
      }
      DeviceClient probe(peer.host, peer.port, peer.password);
      const DeviceInfo remote = probe.info();
      if (peer.id.empty()) peer.id = remote.id;
      if (peer.name.empty()) peer.name = remote.name;
      peer.publicAccess = peer.password.empty() && remote.publicAccess;
      peer.source = "direct";
      state_.addPeer(peer);

      // Share the remote peer's public, directly-connected devices (one hop only).
      for (const Peer& candidate : probe.peers()) {
        if (!candidate.publicAccess || candidate.source != "direct") continue;
        if (candidate.id == state_.info().id) continue;
        if (candidate.host == state_.config().host && candidate.port == state_.config().port) {
          continue;
        }
        Peer shared = candidate;
        shared.source = "shared";
        state_.addPeer(shared);
      }
      return net::HttpResponse::json(200, success(peer.toJson(!loopback(request))).dump());
    }

    if (method == "DELETE" && path.rfind("/api/v1/peers/", 0) == 0) {
      const std::string id =
          net::urlDecode(path.substr(std::string("/api/v1/peers/").size()));
      state_.removePeer(id);
      return net::HttpResponse::json(200, success().dump());
    }

    if (method == "GET" && path == "/api/v1/fs") {
      return net::HttpResponse::json(
          200, success(entriesToJson(state_.browse(request.query.at("path")))).dump());
    }

    if (method == "GET" && path == "/api/v1/fs/file") {
      return net::HttpResponse::bytes(200, state_.readFile(request.query.at("path")));
    }

    if (method == "PUT" && path == "/api/v1/fs/file") {
      state_.writeFile(request.query.at("path"), request.body);
      return net::HttpResponse::json(200, success().dump());
    }

    if (method == "POST" && path == "/api/v1/messages") {
      const json body = json::parse(toString(request.body));
      const std::string from = body.value("from", "");
      const uint64_t seq = state_.addMessage(from.empty() ? state_.info().id : from,
                                             body.value("to", ""),
                                             body.value("text", ""));
      return net::HttpResponse::json(200, success({{"seq", seq}}).dump());
    }

    if (method == "GET" && path == "/api/v1/messages") {
      uint64_t since = 0;
      const auto it = request.query.find("since");
      if (it != request.query.end()) since = std::stoull(it->second);
      return net::HttpResponse::json(
          200, success(messagesToJson(state_.messagesSince(since))).dump());
    }

    if (method == "POST" && path == "/api/v1/relay") {
      const Peer target = Peer::fromJson(json::parse(request.query.at("target")));
      const std::string remotePath = request.query.at("path");
      DeviceClient client(target.host, target.port, target.password);
      client.writeFile(remotePath, request.body);
      return net::HttpResponse::json(200, success().dump());
    }

    if (knownPath(path)) return error(405, "method not allowed");
    return error(404, "not found");
  } catch (const std::exception& exception) {
    return error(400, exception.what());
  }
}

void Daemon::handleWs(int fd, const net::HttpRequest& request) {
  net::Socket socket(fd);
  if (!state_.authorized(request.header("x-fmtrans-auth"))) {
    const std::string reply =
        "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    socket.sendAll(reply);
    return;
  }
  const std::string key = request.header("sec-websocket-key");
  if (key.empty()) return;
  std::string handshake =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: " +
      net::ws::acceptKey(key) + "\r\n\r\n";
  if (!socket.sendAll(handshake)) return;
  // Keep the live stream open while the client is idle.
  socket.setTimeouts(0);

  Bytes buffer = request.extra;
  state_.subscribe(fd);
  while (true) {
    const auto frame = net::ws::recvFrame(fd, buffer);
    if (frame.close) break;
  }
  state_.unsubscribe(fd);
}

bool Daemon::bind() {
  state_.load();
  saveLocalConfig(state_.config());
  std::cout << "listening on " << state_.config().host << ":"
            << state_.config().port << std::endl;
  return server_.bind();
}

bool Daemon::serve() { return server_.serve(); }

bool Daemon::run() {
  if (!bind()) return false;
  return serve();
}

}  // namespace fmtrans
