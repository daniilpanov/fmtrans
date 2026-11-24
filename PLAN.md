# План реализации fmtrans

Версия: 1.0 · Дата: 2026-11-20

> Статус: все этапы (0–8) реализованы. `ctest`: unit + integration + cli — зелёный.
> Детальные результаты и команды запуска — в `README.md`.

## 1. Цель и принципы

Система связи устройств по сети с единым API взаимодействия. Ключевое требование:
**локальное устройство — это такой же узел графа, как и удалённые**, и interacts через
тот же самый клиентский интерфейс (`IDevice` / `DeviceClient`) по `host:port`.
Локальный демон поднимается на `127.0.0.1:<port>`; CLI обращается к нему ровно так же,
как к любому удалённому устройству.

### Решения (приняты самостоятельно)
| Вопрос | Решение |
|---|---|
| Язык / стандарт | C++17, GCC 15 |
| Сборка | CMake + CTest, без тяжёлых зависимостей |
| Транспорт команд | HTTP/1.1 (JSON, бинарные тела) — надёжность, простота тестов |
| Транспорт событий | WebSocket (текстовые кадры) — живые сообщения/события |
| JSON | vendored `nlohmann/json.hpp` (single header, скачивается в репозиторий) |
| Сокеты | POSIX (Linux), собственный минимальный HTTP-стек без внешних либ |
| Хранилище | JSON-файлы + дерево файлов в `--root` |
| CLI | русский UI, args-парсер свой |
| Имена в коде | английский, ASCII, комментарии лаконичные |
| Коммиты | кратко, на английском, без «воды» |

## 2. Архитектура

```
                    +-------------------+
                    |      CLI (ru)     |
                    +---------+---------+
                              | DeviceClient (HTTP) — ОДИНАКОВО для local/remote
        +---------------------+---------------------+
        |                     |                     |
  +-----v------+        +-----v------+        +-----v------+
  | fmtransd A |<------>| fmtransd B |<------>| fmtransd C |
  | 127.0.0.1  |  HTTP  | 10.0.0.2   |  HTTP  | 10.0.0.3   |
  +------------+        +------------+        +------------+
       ^                      ^                     ^
       |  каждый узел держит DeviceClient-ы к своим peer-ам
       +----------------------+---------------------+
                     (граф соединений, relay через B)
```

### Слои
1. `net` — TCP-сокеты, HTTP сервер/клиент, WebSocket-сервер (handshake + frame).
2. `json` — nlohmann/json.
3. `core` — `IDevice`, `DeviceClient`, `NodeState` (конфиг, пиры, сообщения, файлы).
4. `server` — `Daemon`: маршрутизация HTTP-запросов к `NodeState`, WS-рассылка.
5. `app` — `fmtransd` (демон) и `fmtrans` (CLI).

## 3. Единый API устройства (`IDevice`)

Методы (реализуются `DeviceClient` поверх HTTP; сервер предоставляет те же endpoints):

| Метод IDevice | HTTP endpoint | Назначение |
|---|---|---|
| `info()` | `GET /api/v1/info` | id, имя, версия, публичность |
| `listPeers()` | `GET /api/v1/peers` | подключённые + обнаруженные устройства |
| `connect(peer)` | `POST /api/v1/peers` | подключить узел к peer (ip, port, password) |
| `disconnect(id)` | `DELETE /api/v1/peers/{id}` | отключить |
| `browse(path)` | `GET /api/v1/fs?path=...` | листинг каталога (JSON) |
| `readFile(path)` | `GET /api/v1/fs/file?path=...` | бинарное тело файла |
| `writeFile(path, bytes)` | `PUT /api/v1/fs/file?path=...` | запись файла |
| `sendMessage(text, to?)` | `POST /api/v1/messages` | текстовое сообщение |
| `readMessages(since?)` | `GET /api/v1/messages?since=...` | лента сообщений |
| `relay(target, path, bytes)` | `POST /api/v1/relay` | передача файла через этот узел дальше |

Все запросы несут `X-Fmtrans-Auth: <password>` (если узел защищён).
Локальный демон: CLI берёт пароль из `~/.fmtrans/local.conf`, поэтому `fmtrans devices`
для `@local` работает идентично удалённому.

### Формат peer-дескриптора
```json
{"id":"uuid","name":"laptop","host":"127.0.0.1","port":9100,"public":true,
 "source":"direct|shared","password":"***"}
```
`public=true` <=> нет пароля. Такие узлы попадают в шеринг.

## 4. Требования -> реализация

### 4.1 Подключение по IP:port
- `fmtrans connect 10.0.0.2:9100 [--password P] [--name N]` -> CLI создаёт `DeviceClient`,
  вызывает `info()` (проверка доступности/пароля), затем `connect()` на локальном демоне.
- Демон сохраняет пира в `state.json`, создаёт к нему `DeviceClient`, делает initial sync.

### 4.2 Подключение по паролю
- Демон запускается с `--password SECRET`; все запросы без/с неверным токеном -> `401`.
- Пароль пира сохраняется в его дескрипторе и используется при повторных вызовах.
- Хранение в JSON (в реальном продукте — keyring; здесь достаточно для тестов).

### 4.3 Шэринг публичных устройств
- При `connect()` демон A запрашивает у B `GET /peers`, фильтрует `public==true`,
  добавляет их себе со `source=shared` (не автоконнект, только каталог).
- `fmtrans devices` показывает колонку «источник»: прямое / из шеринга.
- Рекурсивный запрет: шеринг только на 1 переход (не распространяем `source=shared`),
  чтобы не зациклиться.

### 4.4 Передача файлов/папок
- Файл: `PUT /fs/file?path=rel/path` (тело = байты), директории создаются автоматически.
- Папка: CLI рекурсивно обходит локальный каталог и делает `writeFile()` для каждого файла
  (относительные пути сохраняются). Скачивание: `GET /fs?path=...` возвращает дерево
  с размерами -> CLI $^2 создаёт структуру и тянет каждый файл.
- Защита от `../`: путь нормализуется и обязан оставаться внутри `--root`.

### 4.5 Текстовые сообщения
- `POST /messages {text, to}`: сообщение сохраняется на целевом узле, рассылается WS-клиентам.
- `GET /messages?since=N`: выборка с seq-курсором. `fmtrans msg`, `fmtrans msgs`.
- WS-событие `{"type":"message","from":...,"text":...}` для live-режима `fmtrans events`.

### 4.6 Просмотр файлов
- `fmtrans browse @local /path` и `fmtrans browse peer_alias /path` — один и тот же вызов
  `IDevice::browse()`. Возвращает имя/тип/размер/mtime. Запрет выхода за root.

### 4.7 Relay через «3-е устройство», локальный = удалённый
- `fmtrans relay via=B to=C ./file /remote/file`: CLI читает файл и вызывает
  `DeviceClient(via).relay(target=C, path, bytes)`.
- Узел `via` в `POST /relay`: находит свой `DeviceClient` к C и вызывает тот же
  `IDevice::writeFile()` — то есть через один и тот же интерфейс.
- Локальный узел подключается к графу так же, как удалённый; в тестах проверяется,
  что `DeviceClient("@local")` и `DeviceClient(remote)` возвращают одинаковый JSON `info`
  (по полю `kind` distinguished только имя/id).

## 5. Протокол HTTP (кратко)

```
GET  /api/v1/info
GET  /api/v1/peers
POST /api/v1/peers            {"host","port","password","name"}
DELETE /api/v1/peers/{id}
GET  /api/v1/fs?path=
GET  /api/v1/fs/file?path=    -> bytes
PUT  /api/v1/fs/file?path=    <- bytes
POST /api/v1/messages         {"text","to"}
GET  /api/v1/messages?since=
POST /api/v1/relay            {"targetId"|"targetAddr","path"} + bytes
GET  /ws                      Upgrade: websocket
```
Ответы: `{"ok":true,"data":...}` / `{"ok":false,"error":"..."}`; ошибки 400/401/404/500.

## 6. CLI (русский интерфейс)

```
fmtrans status                       -- состояние локального узла
fmtrans listen --port N --name N [--password P] [--root DIR] [--fg]
fmtrans daemon start|stop|status     -- фон (pid-файл, nohup/detach)
fmtrans devices                      -- список устройств (direct/shared)
fmtrans connect HOST:PORT [--password P] [--name N]
fmtrans disconnect ID
fmtrans browse DEVICE [PATH]
fmtrans put DEVICE LOCAL REMOTE      -- файл или папка
fmtrans get DEVICE REMOTE LOCAL
fmtrans msg DEVICE TEXT
fmtrans msgs DEVICE [--since N]
fmtrans relay VIA TO LOCAL REMOTE
fmtrans events DEVICE                -- live WS-лента
fmtrans --help
```
`DEVICE` = `@local` | id | имя | `host:port`.

## 7. Тестирование (на каждом коммите)

- **Unit** (`tests/unit.cpp`): JSON round-trip, HTTP request/response parser, path safety,
  auth, CLI args parse, WS frame encode/decode.
- **Integration** (`tests/integration.cpp`): поднимает 3 демона на свободных портах,
  ждёт готовности, прогоняет сценарии через `DeviceClient`:
  1. info/local parity, 2. connect + wrong password (401), 3. public sharing A->B->C,
  4. browse, 5. put/get файла, 6. put/get папки, 7. msg/msgs + WS event,
  8. relay A->B->C, 9. disconnect, 10. path traversal rejection.
- **CI скрипт** `scripts/test.sh`: cmake build + ctest; `scripts/check.sh` перед каждым коммитом.
- Каждый коммит проходит `ctest --output-on-failure`; коммит без зелёных тестов не делается.

## 8. Этапы и коммиты

| # | Коммит | Содержание | Тест-гейт |
|---|---|---|---|
| 0 | `chore: init repo` | CMake, .gitignore, vendored json, PLAN.md | build |
| 1 | `feat: http core` | TCP/HTTP client+server, WS frames, unit тесты | unit |
| 2 | `feat: device api` | IDevice, DeviceClient, NodeState, info/peers | unit+integ#1 |
| 3 | `feat: auth and share` | пароль, 401, шеринг публичных пиров | integ#2,#3 |
| 4 | `feat: filesystem` | browse, read/write, folders, path safety | integ#4,#5,#6,#10 |
| 5 | `feat: messages` | POST/GET messages, WS рассылка | integ#7 |
| 6 | `feat: relay` | POST /relay, local=remote parity | integ#8,#1 |
| 7 | `feat: cli and daemon` | русский CLI, daemon start/stop, events | integ#9 + all |
| 8 | `docs: finalize` | README (ru), final full ctest | all |

## 9. Риски и меры
- **Зацикливание шеринга** — только 1 hop, `source=shared` не распространяется.
- **Обход путей** — нормализация + проверка префикса root.
- **Зависшие демоны в тестах** — RAII-обёртка убивает процесс в деструкторе.
- **Порты** — выбор свободного порта через bind(:0), затем QUIT-механизм.
- **Большие файлы** — потоковое чтение/запись по чанкам, лимит размера запроса.

## 10. Определение готовности
Все 7 требований покрыты интеграционными тестами; `ctest` зелёный; CLI на русском;
код и коммиты на английском ASCII; демон работает в фоне; локальный узел использует
тот же `IDevice`, что и удалённые.

## 11. План рефакторинга (3 коммита)

Каждый пункт — отдельный коммит; после каждого `ctest` должен быть зелёным.

### П1. Байты вместо строк (коммит 1)
- Ввести `using Bytes = std::vector<std::uint8_t>` как единый тип полезной нагрузки.
- HTTP `HttpRequest::body/extra`, `HttpResponse::body`, тело запроса клиента — `Bytes`.
- WebSocket `Frame::payload` и буфер чтения — `Bytes`.
- `IDevice::readFile`/`writeFile`, `NodeState`, файловый IO в CLI — `Bytes`.
- `Socket::recvSome`/`sendAll` работают с `Bytes` (текстовые хелперы — отдельно).
- JSON остаётся текстом и сериализуется в `Bytes` на проводе.
- Тест-гейт: unit + integration + cli.

### П2. Стриминг (коммит 2)
- Ввести `ByteReader`/`ByteWriter` (источник/приёмник байтов) для файлов и сокетов.
- Сервер: заголовки читаются отдельно; тело запроса либо буферизируется (JSON, лимит),
  либо стримится с сокета (`PUT /fs/file`, `POST /relay`).
- `GET /fs/file` отдаёт файл потоково, без загрузки в память.
- Клиент `HttpClient` шлёт/принимает тело потоково; `DeviceClient` использует
  потоковые интерфейсы.
- Relay — сквозной стриминг: источник → узел `via` → целевой узел → файл.
- Тест-гейт: unit + integration + cli + большой файл (> лимит буферизации).

### П3. Архитектура и ОС (коммит 3)
- Проверить отсутствие endianness/alignment-зависимостей (arm/x86).
- Вынести POSIX-специфику (сокеты, процессы, время, права файлов, temp-каталоги)
  в платформенный слой с реализациями для Linux и Windows (`#ifdef _WIN32`).
- Windows-ветки не компилируются в Linux-CI: отметить в README, не ломать Linux.
- Если полный порт потребует гигантских правок — зафиксировать обоснование и объём.
- Тест-гейт: unit + integration + cli на Linux.
