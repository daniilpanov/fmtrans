#!/usr/bin/env bash
# CLI-level integration test: real daemon processes driven by the Russian CLI.
set -euo pipefail

CLI="${1:-./build/fmtrans}"
ROOT="$(mktemp -d "${TMPDIR:-/tmp}/fmtrans-cli-XXXXXX")"
trap 'for h in "$ROOT"/a "$ROOT"/b "$ROOT"/c "$ROOT"/d; do FMTRANS_HOME="$h" "$CLI" daemon stop >/dev/null 2>&1 || true; done; rm -rf "$ROOT"' EXIT

free_port() {
  python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()'
}

PA="$(free_port)"; PB="$(free_port)"; PC="$(free_port)"; PD="$(free_port)"
mkdir -p "$ROOT/a" "$ROOT/b" "$ROOT/c" "$ROOT/d"

export FMTRANS_HOME="$ROOT/a"; "$CLI" daemon start --port "$PA" --name alpha
export FMTRANS_HOME="$ROOT/b"; "$CLI" daemon start --port "$PB" --name beta
export FMTRANS_HOME="$ROOT/c"; "$CLI" daemon start --port "$PC" --name gamma
export FMTRANS_HOME="$ROOT/d"; "$CLI" daemon start --port "$PD" --name delta --password sekret

assert_contains() {
  local text="$1" needle="$2" label="$3"
  if [[ "$text" != *"$needle"* ]]; then
    echo "FAIL $label: expected '$needle' in: $text" >&2
    exit 1
  fi
}

assert_not_contains() {
  local text="$1" needle="$2" label="$3"
  if [[ "$text" == *"$needle"* ]]; then
    echo "FAIL $label: did not expect '$needle' in: $text" >&2
    exit 1
  fi
}

export FMTRANS_HOME="$ROOT/a"
STATUS="$("$CLI" status)"
assert_contains "$STATUS" "alpha" "status name"
assert_contains "$STATUS" "публичное" "status access"

export FMTRANS_HOME="$ROOT/b"
"$CLI" connect "127.0.0.1:$PC" >/dev/null
export FMTRANS_HOME="$ROOT/a"
"$CLI" connect "127.0.0.1:$PB" >/dev/null

export FMTRANS_HOME="$ROOT/a"
DEVICES="$("$CLI" devices)"
assert_contains "$DEVICES" "beta" "devices direct"
# REVERTME: auto-sharing disabled, gamma must NOT appear via sharing.
assert_not_contains "$DEVICES" "gamma" "devices no shared"

printf 'privet cli' > "$ROOT/source.txt"
"$CLI" put "127.0.0.1:$PB" "$ROOT/source.txt" "cli/file.txt" >/dev/null
BROWSE="$("$CLI" browse "127.0.0.1:$PB" cli)"
assert_contains "$BROWSE" "file.txt" "browse"
"$CLI" get "127.0.0.1:$PB" "cli/file.txt" "$ROOT/result.txt" >/dev/null
[[ "$(cat "$ROOT/result.txt")" == "privet cli" ]] || { echo "FAIL get content" >&2; exit 1; }

"$CLI" msg "127.0.0.1:$PB" "privet soobshchenie"
MSGS="$("$CLI" msgs "127.0.0.1:$PB")"
assert_contains "$MSGS" "privet soobshchenie" "messages"

FMTRANS_HOME="$ROOT/b" "$CLI" disconnect "127.0.0.1:$PC" >/dev/null
DEVICES_B="$(FMTRANS_HOME="$ROOT/b" "$CLI" devices)"
if [[ "$DEVICES_B" == *"gamma"* ]]; then
  echo "FAIL disconnect" >&2
  exit 1
fi

# Folder transfer in both directions.
mkdir -p "$ROOT/folder/inner"
printf 'odin' > "$ROOT/folder/one.txt"
printf 'dva' > "$ROOT/folder/inner/two.txt"
FMTRANS_HOME="$ROOT/a" "$CLI" put "127.0.0.1:$PB" "$ROOT/folder" "book" >/dev/null
[[ "$(cat "$ROOT/b/data/book/one.txt")" == "odin" ]] || { echo "FAIL folder put" >&2; exit 1; }
[[ "$(cat "$ROOT/b/data/book/inner/two.txt")" == "dva" ]] || { echo "FAIL nested put" >&2; exit 1; }
FMTRANS_HOME="$ROOT/a" "$CLI" get "127.0.0.1:$PB" book "$ROOT/downloaded" >/dev/null
[[ "$(cat "$ROOT/downloaded/one.txt")" == "odin" ]] || { echo "FAIL folder get" >&2; exit 1; }

# Password-protected peer and a wrong-password rejection.
if FMTRANS_HOME="$ROOT/a" "$CLI" connect "127.0.0.1:$PD" --password wrong >/dev/null 2>&1; then
  echo "FAIL wrong password accepted" >&2
  exit 1
fi
FMTRANS_HOME="$ROOT/a" "$CLI" connect "127.0.0.1:$PD" --password sekret >/dev/null
DEVICES_A="$(FMTRANS_HOME="$ROOT/a" "$CLI" devices)"
assert_contains "$DEVICES_A" "delta" "password peer"
assert_contains "$DEVICES_A" "по паролю" "password access"

# Live WebSocket event stream.
timeout 5 env FMTRANS_HOME="$ROOT/a" "$CLI" events "127.0.0.1:$PB" > "$ROOT/events.txt" 2>&1 &
EVENT_PID=$!
sleep 1
FMTRANS_HOME="$ROOT/a" "$CLI" msg "127.0.0.1:$PB" "zhivoe sobytie" >/dev/null
wait "$EVENT_PID" || true
assert_contains "$(cat "$ROOT/events.txt")" "zhivoe sobytie" "events"

DAEMON_STATUS="$(FMTRANS_HOME="$ROOT/a" "$CLI" daemon status)"
assert_contains "$DAEMON_STATUS" "запущен" "daemon status"

echo "cli: all tests passed"
