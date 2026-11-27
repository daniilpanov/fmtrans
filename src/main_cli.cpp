#include "fmtrans/args.hpp"
#include "fmtrans/core.hpp"
#include "fmtrans/daemon.hpp"
#include "fmtrans/net.hpp"
#include "fmtrans/platform.hpp"

#if defined(_WIN32)
#error "Windows port not yet implemented: main_cli.cpp uses POSIX process APIs"
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>

using namespace fmtrans;

namespace fs = std::filesystem;

namespace {

void printHelp() {
  std::cout << R"(fmtrans - обмен данными между устройствами

Использование:
  fmtrans status                       состояние локального устройства
  fmtrans listen --port N --name N     запустить демон в текущем терминале
                 [--password P] [--root DIR] [--host H]
  fmtrans daemon start|stop|status     управление фоновым демоном
                 [--port N] [--name N] [--password P] [--root DIR] [--host H]
  fmtrans devices                      список известных устройств
  fmtrans connect HOST:PORT            подключить устройство
                 [--password P] [--name N]
  fmtrans disconnect ID                отключить устройство
  fmtrans browse УСТРОЙСТВО [ПУТЬ]     просмотр файлов
  fmtrans put УСТРОЙСТВО ЛОКАЛЬНЫЙ УДАЛЁННЫЙ
                                       передать файл или папку
  fmtrans get УСТРОЙСТВО УДАЛЁННЫЙ ЛОКАЛЬНЫЙ
                                       получить файл или папку
  fmtrans msg УСТРОЙСТВО ТЕКСТ         отправить текстовое сообщение
  fmtrans msgs УСТРОЙСТВО [--since N] [--only-from] [--only-to] [--last-message]   прочитать сообщения
  fmtrans events УСТРОЙСТВО            живая лента событий
  fmtrans --help                       эта справка

УСТРОЙСТВО: @local, идентификатор, имя или HOST:PORT.

Примечание: команда relay удалена из функционала.
)";
}

LocalConfig configFromArgs(const Args& args) {
  LocalConfig config = loadLocalConfig();
  if (args.has("host")) config.host = args.get("host");
  if (args.has("port")) config.port = static_cast<uint16_t>(std::stoul(args.get("port")));
  if (args.has("name")) config.name = args.get("name");
  if (args.has("password")) config.password = args.get("password");
  if (args.has("root")) config.root = args.get("root");
  config.root = fs::absolute(config.root).string();
  return config;
}

struct Target {
  std::string host;
  uint16_t port = 0;
  std::string password;
  std::shared_ptr<IDevice> device;
};

Target resolveTarget(const std::string& key, const LocalConfig& config,
                     DeviceClient& local) {
  if (key == "@local") {
    Target target;
    target.host = config.host;
    target.port = config.port;
    target.password = config.password;
    target.device = std::make_shared<DeviceClient>(config.host, config.port,
                                                   config.password);
    return target;
  }

  for (const Peer& peer : local.peers()) {
    if (peer.id == key || peer.name == key || peer.address() == key) {
      Target target;
      target.host = peer.host;
      target.port = peer.port;
      target.password = peer.password;
      target.device = std::make_shared<DeviceClient>(peer.host, peer.port, peer.password);
      return target;
    }
  }

  const size_t colon = key.rfind(':');
  if (colon != std::string::npos) {
    try {
      Target target;
      target.host = key.substr(0, colon);
      target.port = static_cast<uint16_t>(std::stoul(key.substr(colon + 1)));
      target.device = std::make_shared<DeviceClient>(target.host, target.port);
      return target;
    } catch (const std::exception&) {
      // Fall through to the "not found" error.
    }
  }
  throw Error("устройство не найдено: " + key);
}

std::string trimPath(const std::string& path) {
  size_t begin = 0;
  while (begin < path.size() && path[begin] == '/') ++begin;
  std::string result = path.substr(begin);
  // Normalize "." and "./" to empty (root directory).
  if (result == "." || result == "./") return "";
  return result;
}

std::string joinPath(const std::string& base, const std::string& name) {
  if (base.empty()) return name;
  return base + "/" + name;
}

void putRecursive(IDevice& device, const fs::path& localPath, const std::string& remotePath) {
  if (fs::is_directory(localPath)) {
    for (const auto& item : fs::directory_iterator(localPath)) {
      putRecursive(device, item.path(),
                   joinPath(remotePath, item.path().filename().string()));
    }
    return;
  }
  std::ifstream input(localPath, std::ios::binary);
  const std::string text{std::istreambuf_iterator<char>(input),
                         std::istreambuf_iterator<char>()};
  const Bytes data = toBytes(text);
  device.writeFile(remotePath, data);
  std::cout << "Отправлено: " << remotePath << " (" << data.size() << " байт)\n";
}

void getRecursive(IDevice& device, const std::string& remotePath,
                  const fs::path& localPath) {
  std::vector<FileEntry> entries;
  try {
    entries = device.browse(remotePath);
  } catch (const std::exception&) {
    const Bytes data = device.readFile(remotePath);
    fs::create_directories(localPath.parent_path());
    std::ofstream output(localPath, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    std::cout << "Получено: " << localPath.string() << " (" << data.size() << " байт)\n";
    return;
  }
  fs::create_directories(localPath);
  for (const auto& entry : entries) {
    getRecursive(device, joinPath(remotePath, entry.name),
                 localPath / entry.name);
  }
}

void printMessages(const std::vector<Message>& messages, const DeviceClient& local) {
  for (const auto& message : messages) {
    // Показываем имя отправителя вместо идентификатора.
    std::string fromName = message.from;
    for (const auto& peer : local.peers()) {
      if (peer.id == message.from) { fromName = peer.name; break; }
    }
    std::cout << "[" << message.seq << "] " << message.ts << " "
              << (message.from.empty() ? "?" : fromName);
    if (!message.to.empty()) {
      std::string toName = message.to;
      for (const auto& peer : local.peers()) {
        if (peer.id == message.to) { toName = peer.name; break; }
      }
      std::cout << " -> " << toName;
    }
    std::cout << ": " << message.text << "\n";
  }
}

void watchEvents(const Target& target) {
  net::Socket socket;
  if (!socket.connectTo(target.host, target.port, 3000)) {
    throw Error("не удалось подключиться к устройству");
  }
  std::string request =
      "GET /ws HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n";
  if (!target.password.empty()) {
    request += "X-Fmtrans-Auth: " + target.password + "\r\n";
  }
  request += "\r\n";
  if (!socket.sendAll(request)) throw Error("не удалось отправить запрос");
  Bytes response;
  while (true) {
    if (!socket.recvSome(response)) throw Error("нет ответа устройства");
    const std::string head = toString(response);
    if (head.find("\r\n\r\n") != std::string::npos) break;
  }
  const std::string head = toString(response);
  if (head.find("101 Switching Protocols") == std::string::npos) {
    throw Error("не удалось открыть ленту событий");
  }
  const size_t headerEnd = head.find("\r\n\r\n") + 4;
  Bytes buffer(response.begin() + headerEnd, response.end());
  // Stay connected while idle; the stream ends only when the peer closes it.
  socket.setTimeouts(0);
  std::cout << "Лента событий открыта. Ctrl+C для выхода.\n" << std::flush;
  while (true) {
    const auto frame = net::ws::recvFrame(socket.get(), buffer);
    if (frame.close) break;
    try {
      const json event = json::parse(toString(frame.payload));
      const std::string type = event.value("type", "");
      if (type == "message") {
        const json message = event.at("message");
        std::cout << "Сообщение от " << message.value("from", "?") << ": "
                  << message.value("text", "") << "\n"
                  << std::flush;
      } else {
        std::cout << "Событие: " << event.dump() << "\n" << std::flush;
      }
    } catch (const std::exception&) {
      // Ignore malformed frames.
    }
  }
}

std::string pidPath() { return homeDir() + "/daemon.pid"; }

int readPid() {
  std::ifstream input(pidPath());
  int pid = 0;
  input >> pid;
  return pid;
}

bool isDaemonProcess(int pid) {
#if defined(_WIN32)
  return platform::processExists(pid);
#else
  if (pid <= 0) return false;
  if (::kill(pid, 0) != 0) return false;
  const std::string cmdline = platform::processCmdline(pid);
  if (cmdline.empty()) return true;
  // The background daemon keeps the CLI command line ("fmtrans daemon start ...").
  return cmdline.find("fmtrans") != std::string::npos &&
         cmdline.find("daemon") != std::string::npos;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  const Args args = Args::parse(argc, argv);
  if (args.positional.empty() || args.has("help")) {
    printHelp();
    return 0;
  }
  const std::string command = args.positional[0];
  const auto arg = [&](size_t index) -> std::string {
    return index < args.positional.size() ? args.positional[index] : "";
  };

  try {
    if (command == "listen") {
      Daemon daemon(configFromArgs(args));
      return daemon.run() ? 0 : 1;
    }

    if (command == "daemon") {
      const std::string action = arg(1);
      if (action == "start") {
        if (isDaemonProcess(readPid())) {
          std::cout << "Демон уже запущен.\n";
          return 0;
        }
        const LocalConfig config = configFromArgs(args);
        int pipeFd[2];
        if (::pipe(pipeFd) != 0) {
          std::cerr << "Не удалось создать канал запуска.\n";
          return 1;
        }
        const pid_t pid = fork();
        if (pid < 0) {
          ::close(pipeFd[0]);
          ::close(pipeFd[1]);
          std::cerr << "Не удалось запустить процесс.\n";
          return 1;
        }
        if (pid == 0) {
          ::close(pipeFd[0]);
          setsid();
          const int devnull = ::open("/dev/null", O_RDWR);
          ::dup2(devnull, STDIN_FILENO);
          ::dup2(devnull, STDOUT_FILENO);
          ::dup2(devnull, STDERR_FILENO);
          ::close(devnull);
          Daemon daemon(config);
          if (!daemon.bind()) _exit(1);
          const char ready = '1';
          const ssize_t written = ::write(pipeFd[1], &ready, 1);
          ::close(pipeFd[1]);
          if (written != 1) _exit(1);
          _exit(daemon.serve() ? 0 : 1);
        }
        ::close(pipeFd[1]);
        bool started = false;
        for (int waited = 0; waited < 5000; waited += 50) {
          pollfd pfd{pipeFd[0], POLLIN, 0};
          const int ready = ::poll(&pfd, 1, 50);
          if (ready > 0) {
            char reply = 0;
            const ssize_t got = ::read(pipeFd[0], &reply, 1);
            if (got == 1 && reply == '1') started = true;
            break;
          }
          if (ready < 0 && errno != EINTR) break;
        }
        ::close(pipeFd[0]);
        if (!started) {
          ::kill(pid, SIGTERM);
          ::waitpid(pid, nullptr, 0);
          ::unlink(pidPath().c_str());
          std::cerr << "Демон не смог запуститься (порт занят или конфигурация неверна).\n";
          return 1;
        }
        std::ofstream output(pidPath(), std::ios::trunc);
        output << pid;
        std::cout << "Демон запущен (pid " << pid << ").\n";
        return 0;
      }
      if (action == "stop") {
        const int pid = readPid();
        if (!isDaemonProcess(pid)) {
          ::unlink(pidPath().c_str());
          std::cout << "Демон не запущен.\n";
          return 0;
        }
        ::kill(pid, SIGTERM);
        ::waitpid(pid, nullptr, 0);
        ::unlink(pidPath().c_str());
        std::cout << "Демон остановлен.\n";
        return 0;
      }
      if (action == "status") {
        const int pid = readPid();
        std::cout << (isDaemonProcess(pid)
                          ? "Демон запущен (pid " + std::to_string(pid) + ")."
                          : "Демон не запущен.")
                  << "\n";
        return 0;
      }
      throw Error("неизвестное действие: " + action);
    }

    const LocalConfig config = loadLocalConfig();
    DeviceClient local(config.host, config.port, config.password);

    if (command == "status") {
      const DeviceInfo info = local.info();
      std::cout << "Устройство: " << info.name << "\n";
      std::cout << "Идентификатор: " << info.id << "\n";
      std::cout << "Адрес: " << config.host << ":" << config.port << "\n";
      std::cout << "Доступ: "
                << (info.publicAccess ? "публичное" : "по паролю") << "\n";
      std::cout << "Корень файлов: " << config.root << "\n";
      std::cout << "Устройств в списке: " << local.peers().size() << "\n";
      return 0;
    }

    if (command == "devices") {
      const auto peers = local.peers();
      std::cout << std::left << std::setw(14) << "ID" << std::setw(20) << "ИМЯ"
                << std::setw(24) << "АДРЕС" << std::setw(16) << "ИСТОЧНИК"
                << "ДОСТУП\n";
      for (const auto& peer : peers) {
        std::cout << std::left << std::setw(14) << peer.id.substr(0, 12)
                  << std::setw(20) << peer.name
                  << std::setw(24) << peer.address()
                  << std::setw(16) << (peer.source == "shared" ? "шеринг" : "прямое")
                  << (peer.publicAccess ? "публичное" : "по паролю") << "\n";
      }
      if (peers.empty()) std::cout << "Список пуст.\n";
      return 0;
    }

    if (command == "connect") {
      const std::string address = arg(1);
      const size_t colon = address.rfind(':');
      if (colon == std::string::npos) throw Error("укажите HOST:PORT");
      Peer peer;
      peer.host = address.substr(0, colon);
      peer.port = static_cast<uint16_t>(std::stoul(address.substr(colon + 1)));
      peer.password = args.get("password");
      peer.source = "direct";
      // Сначала получаем информацию об устройстве, чтобы знать его настоящее имя и идентификатор.
      const DeviceInfo info = DeviceClient(peer.host, peer.port, peer.password).info();
      if (peer.name.empty()) peer.name = info.name;
      peer.id = info.id;
      peer.publicAccess = info.publicAccess;
      local.addPeer(peer);
      // Обратное подключение: регистрируем локальное устройство на удалённом,
      // чтобы связь была двусторонней. Используем данные ЛОКАЛЬНОГО устройства,
      // а не удалённого — иначе удалённое устройство получит addPeer со своим
      // собственным идентификатором и откажет ("cannot add self as peer").
      const DeviceInfo localInfo = local.info();
      Peer localPeer;
      localPeer.id = localInfo.id;
      localPeer.name = localInfo.name;
      localPeer.publicAccess = localInfo.publicAccess;
      localPeer.host = config.host;
      localPeer.port = config.port;
      localPeer.password = config.password;
      localPeer.source = "direct";
      try {
        DeviceClient(peer.host, peer.port, peer.password).addPeer(localPeer);
      } catch (const std::exception&) {
        // Если удалённое устройство уже знает о нас, или имя занято —
        // это не критично — локальное подключение уже установлено.
      }
      std::cout << "Подключено: " << info.name << " (" << address << ")\n";
      return 0;
    }

    if (command == "disconnect") {
      // Находим устройство в списке пиров, чтобы получить его адрес и пароль
      const std::string disconnectId = arg(1);
      std::string peerHost, peerPort, peerPassword;
      for (const auto& peer : local.peers()) {
        if (peer.id == disconnectId || peer.name == disconnectId || peer.address() == disconnectId) {
          peerHost = peer.host;
          peerPort = std::to_string(peer.port);
          peerPassword = peer.password;
          break;
        }
      }
      local.removePeer(disconnectId);
      // Обратный отключ: удаляем себя из списка удалённого устройства
      if (!peerHost.empty()) {
        try {
          DeviceClient(peerHost, static_cast<uint16_t>(std::stoul(peerPort)), peerPassword)
              .removePeer(local.info().id);
        } catch (const std::exception&) {
          // Если удалённое устройство недоступно — не критично.
        }
      }
      std::cout << "Устройство отключено.\n";
      return 0;
    }

    if (command == "browse") {
      const Target target = resolveTarget(arg(1), config, local);
      const std::string path = trimPath(arg(2));
      const auto entries = target.device->browse(path);
      if (entries.empty()) std::cout << "Папка пуста.\n";
      for (const auto& entry : entries) {
        std::cout << (entry.type == "dir" ? "[папка] " : "[файл]  ")
                  << std::setw(10) << (entry.type == "dir" ? "" : std::to_string(entry.size))
                  << "  " << entry.name << "\n";
      }
      return 0;
    }

    if (command == "put") {
      const Target target = resolveTarget(arg(1), config, local);
      fs::path source = arg(2);
      if (!fs::exists(source)) throw Error("нет такого файла или папки: " + source.string());
      std::string remotePath = trimPath(arg(3));
      // Если удалённый путь заканчивается на / или равен ./, считаем целевой директорией.
      // Для файла добавляем его имя к целевому пути, чтобы сохранить исходное имя.
      bool remoteIsDir = (arg(3) == "." || arg(3) == "./" ||
                          (arg(3).size() > 0 && arg(3).back() == '/'));
      if (remoteIsDir && !fs::is_directory(source)) {
        remotePath += source.filename().string();
      }
      putRecursive(*target.device, source, remotePath);
      return 0;
    }

    if (command == "get") {
      const Target target = resolveTarget(arg(1), config, local);
      std::string remotePath = trimPath(arg(2));
      fs::path localPath = arg(3);
      // Если локальный путь заканчивается на / или равен . или ./, считаем целевой директорией.
      // . и ./ означают текущую папку запуска CLI.
      bool localIsDir = (arg(3) == "." || arg(3) == "./" ||
                         (arg(3).size() > 0 && arg(3).back() == '/'));
      if (localIsDir) {
        if (arg(3) == "." || arg(3) == "./") {
          localPath = fs::current_path();
        } else {
          std::string p = arg(3);
          if (!p.empty() && p.back() == '/') p.pop_back();
          localPath = fs::path(p);
        }
        // Определяем, файл или папка на удалённой стороне, чтобы добавить имя к целевому пути.
        try {
          target.device->browse(remotePath);
        } catch (const std::exception&) {
          // Если brows() не сработал, это может быть файл
        }
        // Всегда добавляем имя удалённого файла/папки к целевому пути
        localPath /= fs::path(remotePath).filename();
      }
      getRecursive(*target.device, remotePath, localPath);
      return 0;
    }

    if (command == "msg") {
      const Target target = resolveTarget(arg(1), config, local);
      std::string text = arg(2);
      for (size_t i = 3; i < args.positional.size(); ++i) text += " " + args.positional[i];
      // Проверяем, что целевое устройство доступно (получаем его идентификатор).
      const std::string toDeviceId = target.device->info().id;
      try {
        target.device->sendMessage(text, toDeviceId, local.info().id);
      } catch (const std::exception& exception) {
        throw Error("не удалось отправить сообщение устройству " + target.host + ":" +
                    std::to_string(target.port) + ": " + exception.what());
      }
      std::cout << "Сообщение отправлено.\n";
      return 0;
    }

    if (command == "msgs") {
      const Target target = resolveTarget(arg(1), config, local);
      const uint64_t since = args.has("since") ? std::stoull(args.get("since")) : 0;
      const bool onlyFrom = args.has("only-from");
      const bool onlyTo = args.has("only-to");
      const bool lastMessage = args.has("last-message");
      if (onlyFrom && onlyTo) throw Error("--only-from и --only-to нельзя указывать одновременно");
      const std::string targetId = target.device->info().id;
      std::vector<Message> messages = target.device->readMessages(since);
      if (onlyFrom) {
        std::vector<Message> filtered;
        for (const auto& m : messages)
          if (m.from == targetId) filtered.push_back(m);
        messages = std::move(filtered);
      }
      if (onlyTo) {
        std::vector<Message> filtered;
        for (const auto& m : messages)
          if (m.to == targetId) filtered.push_back(m);
        messages = std::move(filtered);
      }
      if (lastMessage) {
        if (!messages.empty()) std::cout << messages.back().text << "\n";
        return 0;
      }
      printMessages(messages, local);
      return 0;
    }

    if (command == "events") {
      watchEvents(resolveTarget(arg(1), config, local));
      return 0;
    }

    throw Error("неизвестная команда: " + command);
  } catch (const std::exception& exception) {
    std::cerr << "Ошибка: " << exception.what() << "\n";
    return 1;
  }
}
