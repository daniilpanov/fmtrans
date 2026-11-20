#pragma once

#include "fmtrans/core.hpp"
#include "fmtrans/net.hpp"
#include "fmtrans/state.hpp"

namespace fmtrans {

class Daemon {
 public:
  explicit Daemon(LocalConfig config);

  // Blocks until the server stops.
  bool run();
  NodeState& state() { return state_; }

 private:
  net::HttpResponse handle(const net::HttpRequest& request);
  void handleWs(int fd, const net::HttpRequest& request);
  net::HttpResponse error(int status, const std::string& message) const;

  NodeState state_;
  net::HttpServer server_;
};

}  // namespace fmtrans
