#include "fmtrans/daemon.hpp"

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <thread>

namespace fs = std::filesystem;

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
         path == "/api/v1/fs/file" || path == "/api/v1/messages";
}

}  // namespace

Daemon::Daemon(LocalConfig config)
    : state_(config),
      server_(config.port,
              [this](const net::HttpRequest& request, net::Socket& socket) {
        return handle(request, socket);
      },
              [this](int fd, const net::HttpRequest& request) { handleWs(fd, request); },
              config.host) {}

net::HttpResponse Daemon::error(int status, const std::string& message) const {
  return net::HttpResponse::json(status, json({{"ok", false}, {"error", message}}).dump());
}

net::HttpResponse Daemon::handle(const net::HttpRequest& request,
                                 net::Socket& socket) {
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
      if (request.body.empty() && request.bodyReader) {
        // Chunked body not supported for peer connect; require Content-Length.
        return error(400, "body required");
      }
      Peer peer = Peer::fromJson(json::parse(toString(request.body)));
      if (peer.host.empty() || peer.port == 0) {
        return error(400, "host and port are required");
      }
      peer.source = "direct";

      // Адрес привязки 0.0.0.0 (все интерфейсы) недоступен извне.
      // Если клиент прислал его при обратном подключении, подменяем на
      // реальный адрес источника соединения (request.peer), иначе удалённое
      // устройство подключится к самому себе.
      if (peer.host == "0.0.0.0" && !request.peer.empty()) {
        peer.host = request.peer;
      }

      state_.addPeer(peer);

      return net::HttpResponse::json(200, success(peer.toJson(!loopback(request))).dump());
    }

    if (method == "DELETE" && path.rfind("/api/v1/peers/", 0) == 0) {
      const std::string id =
          net::urlDecode(path.substr(std::string("/api/v1/peers/").size()));
      auto peerOpt = state_.findPeer(id);
      if (!peerOpt) {
        return error(404, "peer not found: " + id);
      }
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
      const std::string relPath = request.query.at("path");
      // Buffered body: serve it in chunks through the streaming writer.
      Bytes body = std::move(request.body);
      size_t offset = 0;
      state_.writeFile(relPath, [body = std::move(body), &offset](Bytes& out, size_t maxBytes) -> size_t {
        if (offset >= body.size()) return 0;
        const size_t got = std::min(maxBytes, body.size() - offset);
        std::copy(body.begin() + offset, body.begin() + offset + got, std::back_inserter(out));
        offset += got;
        return got;
      });
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
  try {
    state_.load();
  } catch (const std::exception&) {
    // If state.json is corrupted, we can't easily reset state_ (it has atomic
    // members that can't be assigned). The load() method catches parse errors
    // internally, so this catch is just a safety net.
  }
  saveLocalConfig(state_.config());

  // При старте проверяем всех известных пиров. Недоступные удаляем из списка,
  // чтобы они не мешали и не повторялись при следующем запуске.
  try {
    auto peers = state_.peers();
    for (const auto& peer : peers) {
      // Хост 0.0.0.0 — адрес привязки ко всем интерфейсам, им нельзя подключаться
      // к другому устройству (попадёт на localhost/себя). Удаляем такие пиры.
      if (peer.host == "0.0.0.0") {
        state_.removePeer(peer.id);
        continue;
      }
      try {
        net::HttpClient client;
        auto result = client.request("GET", peer.host, peer.port, "/api/v1/info",
                                     {{"X-Fmtrans-Auth", peer.password}}, {}, 2000);
        if (!result.ok) {
          state_.removePeer(peer.id);
        }
      } catch (const std::exception&) {
        state_.removePeer(peer.id);
      }
    }
  } catch (const std::exception&) {
    // If anything goes wrong during pruning, continue with whatever peers we have.
  }

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
