#!/usr/bin/env bash

set -euo pipefail

ROOT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
cd "$ROOT_DIR"

BUILD_DIR="${BUILD_DIR:-build}"
CONF_PATH="${CONF_PATH:-$ROOT_DIR/zaver.conf}"

THREADS="${THREADS:-4}"
CONNS="${CONNS:-500}"
DURATION="${DURATION:-30s}"
WARMUP="${WARMUP:-3s}"

BIG_FILE_MB="${BIG_FILE_MB:-256}"
BIG_FILE_PATH_REL="${BIG_FILE_PATH_REL:-big.bin}"

LOG_FILE="${ROOT_DIR}/tests/perf/bench.server.log"
OUT_MD="${OUT_MD:-$ROOT_DIR/tests/perf/results.md}"

need_cmd() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "Error: missing dependency: $1" >&2
        exit 1
    fi
}

need_cmd curl
need_cmd awk
need_cmd grep
need_cmd sed
need_cmd tr
need_cmd date
need_cmd ss
need_cmd setsid
need_cmd wrk

PORT=3000
if [[ -f "$CONF_PATH" ]]; then
    P=$(grep -E '^[[:space:]]*port[[:space:]]*=' "$CONF_PATH" | tail -n 1 | cut -d= -f2 | tr -d ' \t\r')
    if [[ -n "${P:-}" ]]; then
        PORT="$P"
    fi
fi

BIN_PATH=""
if [[ -f "./${BUILD_DIR}/zaver" ]]; then
    BIN_PATH="./${BUILD_DIR}/zaver"
elif [[ -f "./${BUILD_DIR}/src/zaver" ]]; then
    BIN_PATH="./${BUILD_DIR}/src/zaver"
else
    echo "Error: Could not find 'zaver' under BUILD_DIR=${BUILD_DIR}" >&2
    echo "Hint: build with: cmake -S . -B build && cmake --build build" >&2
    exit 1
fi

cleanup() {
    if [[ -n "${SERVER_PID:-}" ]]; then
        kill -TERM -- "-${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

ensure_port_free() {
    local line
    line=$(ss -ltnp 2>/dev/null | grep -E "LISTEN\\s+.*[:.]${PORT}\\b" || true)
    if [[ -z "$line" ]]; then
        return 0
    fi

    if echo "$line" | grep -q "\"zaver\""; then
        echo "Port $PORT already has zaver; killing it for a clean benchmark run." >&2
        local pids
        pids=$(echo "$line" | grep -oE 'pid=[0-9]+' | cut -d= -f2 | sort -u | tr '\n' ' ')
        if [[ -n "${pids:-}" ]]; then
            kill -KILL $pids 2>/dev/null || true
            for _ in $(seq 1 50); do
                if ! ss -ltnp 2>/dev/null | grep -E "LISTEN\\s+.*[:.]${PORT}\\b" | grep -q "\"zaver\""; then
                    return 0
                fi
                sleep 0.1
            done
        fi
        return 0
    fi

    echo "Error: port $PORT is already in use by another process:" >&2
    echo "$line" >&2
    exit 1
}

wait_ready() {
    local url="$1"
    local ready=0
    for _ in $(seq 1 80); do
        local code
        code=$(curl --max-time 1 -o /dev/null -s -w "%{http_code}" "$url" || true)
        if [[ "$code" != "000" ]]; then
            ready=1
            break
        fi
        sleep 0.1
    done
    if [[ "$ready" -ne 1 ]]; then
        echo "Server did not become ready." >&2
        tail -n 200 "$LOG_FILE" || true
        exit 1
    fi
}

ensure_big_file() {
    local path="$ROOT_DIR/html/${BIG_FILE_PATH_REL}"
    if [[ -f "$path" ]]; then
        return 0
    fi
    echo "Creating big file: $path (${BIG_FILE_MB} MiB)" >&2
    dd if=/dev/zero of="$path" bs=1M count="$BIG_FILE_MB" status=none
}

run_wrk_case() {
    local name="$1"
    local url="$2"

    # Warmup
    wrk -t"$THREADS" -c"$CONNS" -d"$WARMUP" "$url" >/dev/null 2>&1 || true

    local out
    out=$(wrk -t"$THREADS" -c"$CONNS" -d"$DURATION" "$url" 2>/dev/null)

    local rps
    local latency
    local transfer

    rps=$(echo "$out" | awk '/Requests\/sec/ {print $2; exit}')
    latency=$(echo "$out" | awk '/Latency/ {print $2; exit}')
    transfer=$(echo "$out" | awk '/Transfer\/sec/ {print $2; exit}')

    echo "| ${name} | ${url} | ${THREADS} | ${CONNS} | ${DURATION} | ${rps:-N/A} | ${latency:-N/A} | ${transfer:-N/A} |"
}

main() {
    ensure_port_free
    ensure_big_file

    rm -f "$LOG_FILE"
    setsid "$BIN_PATH" -c "$CONF_PATH" >"$LOG_FILE" 2>&1 &
    SERVER_PID=$!

    BASE_URL="http://127.0.0.1:${PORT}"

    wait_ready "${BASE_URL}/index.html"

    local ts
    ts=$(date -u +"%Y-%m-%d %H:%M:%S UTC")

    {
        echo "# Zaver Performance Results"
        echo
        echo "- Time: ${ts}"
        echo "- Binary: ${BIN_PATH}"
        echo "- Config: ${CONF_PATH}"
        echo "- Threads: ${THREADS}"
        echo "- Conns: ${CONNS}"
        echo "- Duration: ${DURATION} (warmup ${WARMUP})"
        echo
        echo "| Case | URL | Threads | Conns | Duration | Requests/sec | Latency(avg) | Transfer/sec |"
        echo "|---|---|---:|---:|---:|---:|---:|---:|"
        run_wrk_case "Static small" "${BASE_URL}/index.html"
        run_wrk_case "Static big (${BIG_FILE_MB}MiB)" "${BASE_URL}/${BIG_FILE_PATH_REL}"
        run_wrk_case "CGI" "${BASE_URL}/cgi-bin/hello.sh"
        echo
        echo "> Notes"
        echo "> - This benchmark assumes a single zaver instance owns the port."
        echo "> - For stable results, run on an idle machine and repeat 3 times." 
    } | tee "$OUT_MD"

    echo
    echo "Saved: $OUT_MD" >&2
}

main "$@"
