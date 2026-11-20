#include "fmtrans/args.hpp"
#include "fmtrans/daemon.hpp"

#include <filesystem>
#include <iostream>

using namespace fmtrans;

int main(int argc, char** argv) {
  const Args args = Args::parse(argc, argv);
  LocalConfig config = loadLocalConfig();
  if (args.has("host")) config.host = args.get("host");
  if (args.has("port")) config.port = static_cast<uint16_t>(std::stoul(args.get("port")));
  if (args.has("name")) config.name = args.get("name");
  if (args.has("password")) config.password = args.get("password");
  if (args.has("root")) config.root = args.get("root");
  config.root = std::filesystem::absolute(config.root).string();

  try {
    Daemon daemon(config);
    return daemon.run() ? 0 : 1;
  } catch (const std::exception& exception) {
    std::cerr << "fmtransd: " << exception.what() << std::endl;
    return 1;
  }
}
