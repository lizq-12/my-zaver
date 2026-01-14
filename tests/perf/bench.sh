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

# Repeat each measured case N times and report mean values.
RUNS="${RUNS:-1}"
PAUSE_BETWEEN_RUNS="${PAUSE_BETWEEN_RUNS:-1}"

# wrk defaults can be too aggressive for slow endpoints like CGI; expose timeout.
WRK_TIMEOUT="${WRK_TIMEOUT:-10s}"

# Benchmark modes:
# - suite: run core cases (static small, static big, CGI, 404)
# - scan_conns: scan CONN_LIST for static small
# - scan_threads: scan THREAD_LIST for static small
# - scale_workers: scan WORKER_LIST, restarting server each time, for static small
# - claims: Nginx-style headline checks (C10K, idle keep-alive RSS, single-core QPS, linear scalability hints)
# - full: suite + scan_conns + scan_threads + scale_workers
MODE="${MODE:-full}"
CONN_LIST="${CONN_LIST:-50 100 200 500 1000}"
WORKER_LIST="${WORKER_LIST:-1 2 4}"
THREAD_LIST="${THREAD_LIST:-1 2 4 8}"

# Nginx-style headline checks (best-effort, environment-dependent).
C10K_CONN_LIST="${C10K_CONN_LIST:-1000 2000 5000 10000}"
IDLE_KEEPALIVE_CONNS="${IDLE_KEEPALIVE_CONNS:-10000}"
IDLE_KEEPALIVE_HOLD_SEC="${IDLE_KEEPALIVE_HOLD_SEC:-10}"
P99_TARGET_MS="${P99_TARGET_MS:-5}"
SINGLE_CORE_CONNS="${SINGLE_CORE_CONNS:-2000}"
SINGLE_CORE_THREADS="${SINGLE_CORE_THREADS:-1}"
ULIMIT_NOFILE="${ULIMIT_NOFILE:-}"

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

if ! [[ "$RUNS" =~ ^[0-9]+$ ]] || [[ "$RUNS" -lt 1 ]]; then
    echo "Error: RUNS must be a positive integer, got: $RUNS" >&2
    exit 1
fi

PORT=3000
if [[ -f "$CONF_PATH" ]]; then
    P=$(grep -E '^[[:space:]]*port[[:space:]]*=' "$CONF_PATH" | tail -n 1 | cut -d= -f2 | tr -d ' \t\r')
    if [[ -n "${P:-}" ]]; then
        PORT="$P"
    fi
fi

WORKERS_CONF=""
if [[ -f "$CONF_PATH" ]]; then
    W=$(grep -E '^[[:space:]]*workers[[:space:]]*=' "$CONF_PATH" | tail -n 1 | cut -d= -f2 | tr -d ' \t\r')
    if [[ -n "${W:-}" ]]; then
        WORKERS_CONF="$W"
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

TABLE_HEADER="| Case | URL | Workers | Threads | Conns | Duration | Runs | Requests/sec(mean) | Lat(avg) | Lat(stdev) | Lat(max) | p50 | p90 | p99 | Transfer/sec(mean) | Non2xx(sum) | Sockerr | Notes |"
TABLE_SEP="|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|"

print_table_header() {
    echo "$TABLE_HEADER"
    echo "$TABLE_SEP"
}

print_section() {
    local title="$1"
    echo
    echo "## ${title}"
    echo
}

sum_rss_kb_tree() {
    local pid="$1"
    if ! command -v ps >/dev/null 2>&1; then
        echo ""
        return 0
    fi

    local pids="$pid"
    if command -v pgrep >/dev/null 2>&1; then
        local children
        children=$(pgrep -P "$pid" 2>/dev/null || true)
        if [[ -n "${children:-}" ]]; then
            pids+=" ${children}"
        fi
    fi

    # rss is in KiB.
    ps -o rss= -p $pids 2>/dev/null | awk '{s+=$1} END { if(NR==0) print ""; else print s }'
}

run_idle_keepalive_mem_test() {
    local url_path="$1"
    local target_conns="$2"
    local hold_sec="$3"

    if ! command -v python3 >/dev/null 2>&1; then
        echo "| Idle keep-alive RSS | N/A | N/A | N/A | python3 missing |"
        return 0
    fi

    local rss_before rss_after
    rss_before=$(sum_rss_kb_tree "$SERVER_PID")

    local py_out
    py_out=$(python3 - <<'PY'
import os, socket, time, sys, re

host = '127.0.0.1'
port = int(os.environ.get('ZV_PORT', '3000'))
path = os.environ.get('ZV_PATH', '/index.html')
target = int(os.environ.get('ZV_TARGET', '10000'))
hold = int(os.environ.get('ZV_HOLD', '10'))

req = (f"GET {path} HTTP/1.1\r\n"
       f"Host: {host}:{port}\r\n"
       f"Connection: keep-alive\r\n"
       f"User-Agent: zaver-bench\r\n"
       f"\r\n").encode('ascii', 'strict')

sock_list = []
opened = 0

def recv_until(sock, marker, limit=65536):
    buf = b''
    while marker not in buf:
        chunk = sock.recv(4096)
        if not chunk:
            break
        buf += chunk
        if len(buf) > limit:
            break
    return buf

for i in range(target):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(3)
        s.connect((host, port))
        s.sendall(req)
        hdr = recv_until(s, b"\r\n\r\n")
        # best-effort: drain body if Content-Length exists.
        m = re.search(br"\r\nContent-length:\s*([0-9]+)\r\n", hdr, re.IGNORECASE)
        if m:
            clen = int(m.group(1))
            body = hdr.split(b"\r\n\r\n", 1)[1]
            need = max(0, clen - len(body))
            while need > 0:
                chunk = s.recv(min(4096, need))
                if not chunk:
                    break
                need -= len(chunk)
        s.settimeout(None)
        sock_list.append(s)
        opened += 1
    except OSError:
        break

print(f"opened={opened}")
sys.stdout.flush()
time.sleep(hold)

for s in sock_list:
    try:
        s.close()
    except Exception:
        pass
PY
    )

    rss_after=$(sum_rss_kb_tree "$SERVER_PID")

    local opened
    opened=$(echo "$py_out" | sed -nE 's/^opened=([0-9]+)\s*$/\1/p' | tail -n 1)
    if [[ -z "${opened:-}" ]]; then
        opened=0
    fi

    local delta_kb per_conn_b
    if [[ -n "${rss_before:-}" && -n "${rss_after:-}" ]]; then
        delta_kb=$((rss_after - rss_before))
    else
        delta_kb=""
    fi

    if [[ "$opened" -gt 0 && -n "${delta_kb:-}" ]]; then
        per_conn_b=$(awk -v dk="$delta_kb" -v n="$opened" 'BEGIN{ printf "%.1f", (dk*1024)/n }')
    else
        per_conn_b="N/A"
    fi

    echo "| Idle keep-alive RSS (proc RSS only) | ${target_conns} | ${opened} | ${hold_sec}s | rss_kb_before=${rss_before:-N/A}; rss_kb_after=${rss_after:-N/A}; delta_kb=${delta_kb:-N/A}; bytes_per_conn=${per_conn_b} |"
}

stop_server() {
    if [[ -n "${SERVER_PID:-}" ]]; then
        kill -TERM -- "-${SERVER_PID}" 2>/dev/null || true
        wait "${SERVER_PID}" 2>/dev/null || true
        SERVER_PID=""
    fi
}

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

make_conf_with_workers() {
    local workers="$1"
    local out_path="$2"

    if [[ ! -f "$CONF_PATH" ]]; then
        echo "Error: CONF_PATH not found: $CONF_PATH" >&2
        exit 1
    fi

    if grep -Eq '^[[:space:]]*workers[[:space:]]*=' "$CONF_PATH"; then
        sed -E "s/^[[:space:]]*workers[[:space:]]*=.*/workers=${workers}/" "$CONF_PATH" >"$out_path"
    else
        cat "$CONF_PATH" >"$out_path"
        echo "workers=${workers}" >>"$out_path"
    fi
}

make_conf_with_kv() {
    local key="$1"
    local value="$2"
    local in_path="$3"
    local out_path="$4"

    if grep -Eq "^[[:space:]]*${key}[[:space:]]*=" "$in_path"; then
        sed -E "s/^[[:space:]]*${key}[[:space:]]*=.*/${key}=${value}/" "$in_path" >"$out_path"
    else
        cat "$in_path" >"$out_path"
        echo "${key}=${value}" >>"$out_path"
    fi
}

start_server() {
    local conf="$1"
    if [[ -n "${ULIMIT_NOFILE:-}" ]]; then
        (ulimit -n "$ULIMIT_NOFILE" 2>/dev/null) || true
    fi
    ensure_port_free
    rm -f "$LOG_FILE"
    setsid "$BIN_PATH" -c "$conf" >"$LOG_FILE" 2>&1 &
    SERVER_PID=$!
    wait_ready "http://127.0.0.1:${PORT}/index.html"
}

run_wrk_case() {
    local name="$1"
    local url="$2"

    # Warmup
    wrk --latency --timeout "$WRK_TIMEOUT" -t"$THREADS" -c"$CONNS" -d"$WARMUP" "$url" >/dev/null 2>&1 || true

    # Helpers: parse and normalize wrk units for aggregation.
    to_ms() {
        local v="$1"
        awk -v t="$v" 'BEGIN {
            if (t=="" || t=="N/A") { print ""; exit }
            if (t ~ /us$/) { sub(/us$/, "", t); printf "%.6f", t/1000; exit }
            if (t ~ /ms$/) { sub(/ms$/, "", t); printf "%.6f", t; exit }
            if (t ~ /s$/)  { sub(/s$/,  "", t); printf "%.6f", t*1000; exit }
            printf "";
        }'
    }

    to_mib_per_sec() {
        local v="$1"
        awk -v t="$v" 'BEGIN {
            if (t=="" || t=="N/A") { print ""; exit }
            if (t ~ /KB$/) { sub(/KB$/, "", t); printf "%.6f", t/1024; exit }
            if (t ~ /MB$/) { sub(/MB$/, "", t); printf "%.6f", t; exit }
            if (t ~ /GB$/) { sub(/GB$/, "", t); printf "%.6f", t*1024; exit }
            if (t ~ /B$/)  { sub(/B$/,  "", t); printf "%.6f", t/1024/1024; exit }
            printf "";
        }'
    }

    mean_of() {
        awk '{s+=$1; n+=1} END { if(n==0) print ""; else printf "%.6f", s/n }'
    }

    fmt_ms() {
        local v="$1"
        awk -v x="$v" 'BEGIN { if (x=="" || x=="N/A") print "N/A"; else printf "%.3fms", x }'
    }

    fmt_mibps() {
        local v="$1"
        awk -v x="$v" 'BEGIN { if (x=="" || x=="N/A") print "N/A"; else printf "%.2fMB", x }'
    }

    local rps_list=""
    local lat_avg_ms_list=""
    local lat_stdev_ms_list=""
    local lat_max_ms_list=""
    local p50_ms_list=""
    local p90_ms_list=""
    local p99_ms_list=""
    local xfer_mibps_list=""
    local non2xx_sum=0
    local sockerr_text=""

    for run_i in $(seq 1 "$RUNS"); do
        local out
        out=$(wrk --latency --timeout "$WRK_TIMEOUT" -t"$THREADS" -c"$CONNS" -d"$DURATION" "$url" 2>/dev/null || true)

        local rps
        local latency_avg
        local latency_stdev
        local latency_max
        local transfer
        local non2xx
        local socket_errors

        rps=$(echo "$out" | awk '/Requests\/sec/ {print $2; exit}')
        latency_avg=$(echo "$out" | awk '/Latency/ {print $2; exit}')
        latency_stdev=$(echo "$out" | awk '/Latency/ {print $3; exit}')
        latency_max=$(echo "$out" | awk '/Latency/ {print $4; exit}')
        transfer=$(echo "$out" | awk '/Transfer\/sec/ {print $2; exit}')

        local p50
        local p90
        local p99
        p50=$(echo "$out" | awk 'BEGIN{f=0} /Latency Distribution/ {f=1; next} f && $1=="50%" {print $2; exit}')
        p90=$(echo "$out" | awk 'BEGIN{f=0} /Latency Distribution/ {f=1; next} f && $1=="90%" {print $2; exit}')
        p99=$(echo "$out" | awk 'BEGIN{f=0} /Latency Distribution/ {f=1; next} f && $1=="99%" {print $2; exit}')

        non2xx=$(echo "$out" | awk -F': ' '/Non-2xx or 3xx responses/ {print $2; exit}')
        socket_errors=$(echo "$out" | awk -F': ' '/Socket errors/ {print $2; exit}')

        if [[ -n "${non2xx:-}" ]]; then
            non2xx_sum=$((non2xx_sum + non2xx))
        fi
        if [[ -n "${socket_errors:-}" ]]; then
            sockerr_text="$socket_errors"
        fi

        rps_list+="$(echo "${rps:-0}" | awk '{printf "%.6f", $1}')\n"
        lat_avg_ms_list+="$(to_ms "${latency_avg:-N/A}")\n"
        lat_stdev_ms_list+="$(to_ms "${latency_stdev:-N/A}")\n"
        lat_max_ms_list+="$(to_ms "${latency_max:-N/A}")\n"
        p50_ms_list+="$(to_ms "${p50:-N/A}")\n"
        p90_ms_list+="$(to_ms "${p90:-N/A}")\n"
        p99_ms_list+="$(to_ms "${p99:-N/A}")\n"
        xfer_mibps_list+="$(to_mib_per_sec "${transfer:-N/A}")\n"

        if [[ "$run_i" -lt "$RUNS" ]]; then
            sleep "$PAUSE_BETWEEN_RUNS"
        fi
    done

    local rps_mean
    local lat_avg_ms_mean
    local lat_stdev_ms_mean
    local lat_max_ms_mean
    local p50_ms_mean
    local p90_ms_mean
    local p99_ms_mean
    local xfer_mibps_mean

    rps_mean=$(echo -e "$rps_list" | awk 'NF{print}' | mean_of)
    lat_avg_ms_mean=$(echo -e "$lat_avg_ms_list" | awk 'NF{print}' | mean_of)
    lat_stdev_ms_mean=$(echo -e "$lat_stdev_ms_list" | awk 'NF{print}' | mean_of)
    lat_max_ms_mean=$(echo -e "$lat_max_ms_list" | awk 'NF{print}' | mean_of)
    p50_ms_mean=$(echo -e "$p50_ms_list" | awk 'NF{print}' | mean_of)
    p90_ms_mean=$(echo -e "$p90_ms_list" | awk 'NF{print}' | mean_of)
    p99_ms_mean=$(echo -e "$p99_ms_list" | awk 'NF{print}' | mean_of)
    xfer_mibps_mean=$(echo -e "$xfer_mibps_list" | awk 'NF{print}' | mean_of)

    LAST_RPS_MEAN="$rps_mean"
    LAST_P99_MS_MEAN="$p99_ms_mean"
    LAST_XFER_MIBPS_MEAN="$xfer_mibps_mean"
    LAST_LAT_AVG_MS_MEAN="$lat_avg_ms_mean"

    local notes=""
    if [[ "$non2xx_sum" -ne 0 ]]; then
        notes="non2xx_sum=${non2xx_sum}"
    fi
    if [[ -n "${sockerr_text:-}" ]]; then
        if [[ -n "$notes" ]]; then
            notes+="; "
        fi
        notes+="sockerr(${sockerr_text})"
    fi
    if [[ -z "$notes" ]]; then
        notes="-"
    fi

    echo "| ${name} | ${url} | ${WORKERS_LABEL:-${WORKERS_CONF:-N/A}} | ${THREADS} | ${CONNS} | ${DURATION} | ${RUNS} | ${rps_mean:-N/A} | $(fmt_ms "$lat_avg_ms_mean") | $(fmt_ms "$lat_stdev_ms_mean") | $(fmt_ms "$lat_max_ms_mean") | $(fmt_ms "$p50_ms_mean") | $(fmt_ms "$p90_ms_mean") | $(fmt_ms "$p99_ms_mean") | $(fmt_mibps "$xfer_mibps_mean") | ${non2xx_sum} | ${sockerr_text:-0} | ${notes} |"
}

run_wrk_case_with_conns() {
    local name="$1"
    local url="$2"
    local conns="$3"

    local old_conns="$CONNS"
    CONNS="$conns"
    run_wrk_case "$name" "$url"
    CONNS="$old_conns"
}

main() {
    ensure_big_file
    BASE_URL="http://127.0.0.1:${PORT}"

    local ts
    ts=$(date -u +"%Y-%m-%d %H:%M:%S UTC")

    {
        echo "# Zaver Performance Results"
        echo
        echo "- Time: ${ts}"
        echo "- Binary: ${BIN_PATH}"
        echo "- Config: ${CONF_PATH}"
        echo "- Mode: ${MODE}"
        echo "- Threads: ${THREADS}"
        echo "- Base Conns: ${CONNS}"
        echo "- Duration: ${DURATION} (warmup ${WARMUP})"
        echo "- Runs per case: ${RUNS}"
        echo "- wrk timeout: ${WRK_TIMEOUT}"
        if [[ -n "${WORKERS_CONF:-}" ]]; then
            echo "- workers (from conf): ${WORKERS_CONF}"
        fi

        if [[ "$MODE" == "suite" || "$MODE" == "full" ]]; then
            print_section "Suite"
            print_table_header
            declare -A SUITE_RPS
            declare -A SUITE_P99_MS
            declare -A SUITE_P99_OK

            WORKERS_LABEL="${WORKERS_CONF:-N/A}"
            start_server "$CONF_PATH"
            run_wrk_case "Static small" "${BASE_URL}/index.html"
            SUITE_RPS["Static small"]="${LAST_RPS_MEAN:-}"
            SUITE_P99_MS["Static small"]="${LAST_P99_MS_MEAN:-}"
            SUITE_P99_OK["Static small"]=$(awk -v p99="${LAST_P99_MS_MEAN:-}" -v t="${P99_TARGET_MS}" 'BEGIN{ if(p99==""||p99=="N/A") print "N/A"; else if(p99<=t) print "PASS"; else print "FAIL" }')

            run_wrk_case "Static big (${BIG_FILE_MB}MiB)" "${BASE_URL}/${BIG_FILE_PATH_REL}"
            SUITE_RPS["Static big"]="${LAST_RPS_MEAN:-}"
            SUITE_P99_MS["Static big"]="${LAST_P99_MS_MEAN:-}"
            SUITE_P99_OK["Static big"]=$(awk -v p99="${LAST_P99_MS_MEAN:-}" -v t="${P99_TARGET_MS}" 'BEGIN{ if(p99==""||p99=="N/A") print "N/A"; else if(p99<=t) print "PASS"; else print "FAIL" }')

            run_wrk_case "CGI" "${BASE_URL}/cgi-bin/hello.sh"
            SUITE_RPS["CGI"]="${LAST_RPS_MEAN:-}"
            SUITE_P99_MS["CGI"]="${LAST_P99_MS_MEAN:-}"
            SUITE_P99_OK["CGI"]=$(awk -v p99="${LAST_P99_MS_MEAN:-}" -v t="${P99_TARGET_MS}" 'BEGIN{ if(p99==""||p99=="N/A") print "N/A"; else if(p99<=t) print "PASS"; else print "FAIL" }')

            run_wrk_case "404" "${BASE_URL}/no-such-file"
            SUITE_RPS["404"]="${LAST_RPS_MEAN:-}"
            SUITE_P99_MS["404"]="${LAST_P99_MS_MEAN:-}"
            SUITE_P99_OK["404"]=$(awk -v p99="${LAST_P99_MS_MEAN:-}" -v t="${P99_TARGET_MS}" 'BEGIN{ if(p99==""||p99=="N/A") print "N/A"; else if(p99<=t) print "PASS"; else print "FAIL" }')
            stop_server
            echo

            local base_rps base_p99
            base_rps="${SUITE_RPS["Static small"]:-}"
            base_p99="${SUITE_P99_MS["Static small"]:-}"
            if [[ -n "${base_rps:-}" ]]; then
                echo "## Relative Comparison (vs Static small)"
                echo
                echo "| Case | RPS x | p99 x |"
                echo "|---|---:|---:|"
                for k in "Static small" "Static big" "CGI" "404"; do
                    local rps_k p99_k
                    rps_k="${SUITE_RPS[$k]:-}"
                    p99_k="${SUITE_P99_MS[$k]:-}"

                    local rps_ratio p99_ratio
                    rps_ratio=$(awk -v a="$rps_k" -v b="$base_rps" 'BEGIN { if(a==""||b==""||b==0) print "N/A"; else printf "%.6f", a/b }')
                    p99_ratio=$(awk -v a="$p99_k" -v b="$base_p99" 'BEGIN { if(a==""||b==""||b==0) print "N/A"; else printf "%.3f", a/b }')
                    echo "| ${k} | ${rps_ratio} | ${p99_ratio} |"
                done
                echo
            fi

            echo "## P99 Target Check (<= ${P99_TARGET_MS}ms)"
            echo
            echo "| Case | p99(ms) | Result |"
            echo "|---|---:|---:|"
            for k in "Static small" "Static big" "CGI" "404"; do
                echo "| ${k} | ${SUITE_P99_MS[$k]:-N/A} | ${SUITE_P99_OK[$k]:-N/A} |"
            done
            echo
        fi

        if [[ "$MODE" == "scan_conns" || "$MODE" == "full" ]]; then
            print_section "Conns Scan (Static small)"
            print_table_header
            WORKERS_LABEL="${WORKERS_CONF:-N/A}"
            start_server "$CONF_PATH"
            for c in $CONN_LIST; do
                run_wrk_case_with_conns "Static small" "${BASE_URL}/index.html" "$c"
            done
            stop_server
            echo
        fi

        if [[ "$MODE" == "scan_threads" || "$MODE" == "full" ]]; then
            print_section "Threads Scan (Static small)"
            print_table_header
            local old_threads="$THREADS"
            WORKERS_LABEL="${WORKERS_CONF:-N/A}"
            start_server "$CONF_PATH"
            for t in $THREAD_LIST; do
                THREADS="$t"
                run_wrk_case "Static small" "${BASE_URL}/index.html"
            done
            THREADS="$old_threads"
            stop_server
            echo
        fi

        if [[ "$MODE" == "scale_workers" || "$MODE" == "full" ]]; then
            print_section "Workers Scale (Static small)"
            print_table_header
            local tmp_conf
            tmp_conf="${ROOT_DIR}/tests/perf/_tmp_zaver.conf"
            declare -A SCALE_RPS

            for w in $WORKER_LIST; do
                make_conf_with_workers "$w" "$tmp_conf"
                WORKERS_LABEL="$w"
                start_server "$tmp_conf"
                run_wrk_case "Static small" "${BASE_URL}/index.html"
                SCALE_RPS[$w]="${LAST_RPS_MEAN:-}"
                stop_server
            done
            rm -f "$tmp_conf" || true
            echo

            # Summary: speedup and efficiency (best-effort)
            local base_w
            base_w=$(echo "$WORKER_LIST" | awk '{print $1; exit}')
            local base_rps
            base_rps="${SCALE_RPS[$base_w]:-}"
            if [[ -n "${base_rps:-}" ]]; then
                echo "### Scaling Summary"
                echo
                echo "| Workers | RPS(mean) | Speedup | Efficiency |"
                echo "|---:|---:|---:|---:|"
                for w in $WORKER_LIST; do
                    local r
                    r="${SCALE_RPS[$w]:-}"
                    local speed eff
                    speed=$(awk -v a="$r" -v b="$base_rps" 'BEGIN{ if(a==""||b==""||b==0) print "N/A"; else printf "%.3f", a/b }')
                    eff=$(awk -v s="$speed" -v w="$w" 'BEGIN{ if(s=="N/A"||w==0) print "N/A"; else printf "%.3f", s/w }')
                    echo "| ${w} | ${r:-N/A} | ${speed} | ${eff} |"
                done
                echo
            fi
        fi

        if [[ "$MODE" == "claims" ]]; then
            print_section "C10K / High Concurrency (Static small)"
            print_table_header
            WORKERS_LABEL="${WORKERS_CONF:-N/A}"
            start_server "$CONF_PATH"
            for c in $C10K_CONN_LIST; do
                run_wrk_case_with_conns "Static small" "${BASE_URL}/index.html" "$c"
            done
            echo

            echo "### Idle Keep-Alive Memory (best-effort)"
            echo
            echo "| Metric | Target conns | Opened conns | Hold | Result |"
            echo "|---|---:|---:|---:|---|"
            ZV_PORT="$PORT" ZV_PATH="/index.html" ZV_TARGET="$IDLE_KEEPALIVE_CONNS" ZV_HOLD="$IDLE_KEEPALIVE_HOLD_SEC" \
                run_idle_keepalive_mem_test "/index.html" "$IDLE_KEEPALIVE_CONNS" "$IDLE_KEEPALIVE_HOLD_SEC"
            echo

            stop_server

            print_section "Single-Core QPS (Static small, workers=1)"
            print_table_header
            local tmp_conf_sc tmp1 tmp2
            tmp_conf_sc="${ROOT_DIR}/tests/perf/_tmp_zaver.single_core.conf"
            tmp1="${ROOT_DIR}/tests/perf/_tmp_zaver.single_core.1.conf"
            tmp2="${ROOT_DIR}/tests/perf/_tmp_zaver.single_core.2.conf"
            make_conf_with_workers 1 "$tmp1"
            make_conf_with_kv "cpu_affinity" 1 "$tmp1" "$tmp2"
            mv "$tmp2" "$tmp_conf_sc"

            local old_threads old_conns
            old_threads="$THREADS"
            old_conns="$CONNS"
            THREADS="$SINGLE_CORE_THREADS"
            CONNS="$SINGLE_CORE_CONNS"
            WORKERS_LABEL=1
            start_server "$tmp_conf_sc"
            run_wrk_case "Static small (single-core config)" "${BASE_URL}/index.html"
            stop_server

            THREADS="$old_threads"
            CONNS="$old_conns"
            rm -f "$tmp_conf_sc" "$tmp1" 2>/dev/null || true
            echo
        fi

        echo
        echo "> Notes"
        echo "> - This benchmark assumes a single zaver instance owns the port (SO_REUSEPORT allows multiple instances)."
        echo "> - For stable results, run on an idle machine and set RUNS=3 (or higher)." 
        echo "> - "
        echo "> Nginx-style claims are environment-dependent:" 
        echo "> - "
        echo ">   - C10K/QPS depend on CPU/OS/tuning and client limits (ulimit)."
        echo ">   - Idle keep-alive memory here measures process RSS only (kernel socket memory is not included)."
    } | tee "$OUT_MD"

    echo
    echo "Saved: $OUT_MD" >&2
}

main "$@"
