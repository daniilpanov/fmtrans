# fmtrans

Система связи устройств по IP:port с единым API взаимодействия. Локальное устройство —
такой же узел графа, как и удалённые: CLI обращается к нему через тот же
`DeviceClient`, что и к любым другим устройствам.

## Возможности

1. Подключение по IP:port.
2. Подключение по паролю (неверный пароль -> `401`).
3. Шэринг публичных устройств: при подключении собирается список публических пиров
   соседа (только один переход, без зацикливания).
4. Передача файлов и целых папок (рекурсивно) между устройствами.
5. Текстовые сообщения + живая WS-лента.
6. Просмотр файлов на любом устройстве.

## Сборка

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Или one-liner: `./scripts/test.sh`.

## Запуск

```bash
# демон в фоне
./build/fmtrans daemon start --port 9000 --name laptop --root ~/fmtrans-data

# или в текущем терминале
./build/fmtrans listen --port 9000 --name laptop

./build/fmtrans status
./build/fmtrans daemon stop
```

Настройки хранятся в `$FMTRANS_HOME` (по умолчанию `~/.fmtrans`):
`local.conf` (host/port/name/password/root), `state.json` (пиры и сообщения),
`data/` (файлы), `daemon.pid`. Файлы конфигурации сохраняются с правами `0600`,
домашний каталог — `0700`.

`--host` задаёт адрес, на котором демон слушает (по умолчанию `127.0.0.1`).
Чтобы принимать подключения с других устройств, укажите `--host 0.0.0.0`
или IP-адрес нужного интерфейса.

## Примеры использования

```bash
# подключение к устройству (по паролю --password secret)
fmtrans connect 192.168.1.10:9100 --password secret

# список устройств (прямые и из шеринга)
fmtrans devices

# просмотр файлов и передача
fmtrans browse @local /docs
fmtrans put 192.168.1.10:9100 ./report.pdf /inbox/report.pdf
fmtrans get 192.168.1.10:9100 /inbox/report.pdf ./copy.pdf

# сообщения и живая лента
fmtrans msg 192.168.1.10:9100 "Привет!"
fmtrans msgs 192.168.1.10:9100 --since 5
fmtrans events 192.168.1.10:9100
```

`УСТРОЙСТВО` можно задать как `@local`, идентификатор, имя или `HOST:PORT`.

## Архитектура

```
CLI (ru) --DeviceClient(HTTP)--> fmtransd A --DeviceClient--> fmtransd B ...
```

- `net` — POSIX TCP, HTTP/1.1 клиент/сервер, WebSocket-кадры, SHA1/base64.
- `core` — `IDevice`, `DeviceClient`, типы данных, конфигурация.
- `state` — пиры, сообщения, файлы с защитой от выхода за root, WS-подписчики.
- `daemon` — HTTP-роутер `/api/v1/*` и WS `/ws`.
- `app` — `fmtransd` (демон) и `fmtrans` (CLI).

### Протокол

| Метод | Endpoint | Назначение |
|---|---|---|
| GET | `/api/v1/info` | информация об устройстве |
| GET/POST | `/api/v1/peers` | список / подключение |
| DELETE | `/api/v1/peers/{id}` | отключение |
| GET | `/api/v1/fs?path=` | листинг |
| GET/PUT | `/api/v1/fs/file?path=` | чтение / запись файла |
| POST/GET | `/api/v1/messages` | отправка / чтение сообщений |
| GET | `/ws` | живые события |

Аутентификация: заголовок `X-Fmtrans-Auth: <password>`. публичное устройство —
без пароля.

## Тестирование

- `unit` — SHA1/base64, HTTP-парсер, WS-кадры, query, сериализация.
- `integration` — 3 демона в отдельных домах: parity local/remote, пароль и `401`,
  шеринг, файлы/папки и защита путей, сообщения + WS, отключение.
- `cli` — реальные фоновые демоны и все команды CLI, включая папки, пароль,
  живую ленту.

Все тесты запускаются на каждом коммите (`scripts/test.sh`).
