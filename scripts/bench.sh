#!/usr/bin/env bash
set -euo pipefail

# Zaver benchmark + correctness script
# - Builds
# - Generates a large static file
# - Starts server
# - Verifies sha256 integrity via HTTP download
# - Runs wrk
# - Optionally samples CPU sys% via pidstat

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONF_FILE="${CONF_FILE:-${ROOT_DIR}/zaver.conf}"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"

# Compare mode:
# - COMPARE=0 (default): behave like before, use BUILD_DIR
# - COMPARE=1: build+run twice (cache on/off) in separate build dirs and summarize
COMPARE="${COMPARE:-0}"
BUILD_DIR_CACHE="${BUILD_DIR_CACHE:-${ROOT_DIR}/build_cache}"
BUILD_DIR_NOCACHE="${BUILD_DIR_NOCACHE:-${ROOT_DIR}/build_nocache}"

# Ensure the server's CWD matches zaver.conf relative paths (e.g. root=./html)
cd "${ROOT_DIR}"

PORT="${PORT:-}"
DOCROOT="${DOCROOT:-}"

# Parse zaver.conf (format: key=value)
if [[ -z "${PORT}" ]]; then
  PORT="$(awk -F= '$1=="port"{print $2}' "${CONF_FILE}" | tail -n1)"
fi
if [[ -z "${DOCROOT}" ]]; then
  DOCROOT="$(awk -F= '$1=="root"{print $2}' "${CONF_FILE}" | tail -n1)"
fi

if [[ -z "${PORT}" || -z "${DOCROOT}" ]]; then
  echo "Failed to parse port/root from ${CONF_FILE}" >&2
  exit 1
fi

DOCROOT_ABS="${ROOT_DIR}/${DOCROOT#./}"
URL_BASE="http://127.0.0.1:${PORT}"
BIG_NAME="big_64m.bin"
BIG_PATH="${DOCROOT_ABS}/${BIG_NAME}"

# wrk target (default: small file so allocator effects are visible)
SMALL_NAME="small_4k.bin"
SMALL_PATH="${DOCROOT_ABS}/${SMALL_NAME}"
WRK_PATH="${WRK_PATH:-/${SMALL_NAME}}"

# - 0: default keep-alive (wrk default)
# - 1: force "Connection: close" to create accept/close churn (useful for testing request freelist)
WRK_CONN_CLOSE="${WRK_CONN_CLOSE:-0}"

# - 0: do not request percentile output
# - 1: pass wrk --latency and parse p50/p90/p99
WRK_LATENCY="${WRK_LATENCY:-0}"

THREADS="${THREADS:-4}"
CONNS="${CONNS:-200}"
DURATION="${DURATION:-30s}"

# Repeat mode: run A/B multiple times and summarize avg/median.
REPEAT="${REPEAT:-1}"

# Stress preset: only applies when THREADS/CONNS/DURATION were not explicitly provided.
# - STRESS=1 will set CONNS=1000 and DURATION=120s by default.
STRESS="${STRESS:-0}"

# Track whether user explicitly provided these env vars
USER_THREADS_SET=0
USER_CONNS_SET=0
USER_DURATION_SET=0
if [[ -n "${THREADS:-}" && "${THREADS}" != "4" ]]; then USER_THREADS_SET=1; fi
if [[ -n "${CONNS:-}" && "${CONNS}" != "200" ]]; then USER_CONNS_SET=1; fi
if [[ -n "${DURATION:-}" && "${DURATION}" != "30s" ]]; then USER_DURATION_SET=1; fi

if [[ "${STRESS}" == "1" ]]; then
  if [[ "${USER_CONNS_SET}" == "0" ]]; then CONNS="1000"; fi
  if [[ "${USER_DURATION_SET}" == "0" ]]; then DURATION="120s"; fi
fi

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]]; then
    kill "${SERVER_PID}" >/dev/null 2>&1 || true
    wait "${SERVER_PID}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${PIDSTAT_PID:-}" ]]; then
    kill "${PIDSTAT_PID}" >/dev/null 2>&1 || true
    wait "${PIDSTAT_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

step() {
  echo
  echo "==> $*"
}

parse_wrk_reqps() {
  local wrk_log="$1"
  awk '/Requests\/sec/ {print $2}' "${wrk_log}" | tail -n1
}

parse_wrk_transfer() {
  local wrk_log="$1"
  awk '/Transfer\/sec/ {print $2" "$3}' "${wrk_log}" | tail -n1
}

parse_wrk_latency_line() {
  local wrk_log="$1"
  # Format: Latency   1.23ms   0.45ms  10.00ms
  awk '/^Latency[[:space:]]+/ {print $2" "$3" "$4}' "${wrk_log}" | tail -n1
}

parse_wrk_percentile() {
  local wrk_log="$1"
  local pct="$2"  # e.g. 50, 90, 99
  awk -v p="${pct}%" '$1==p {print $2}' "${wrk_log}" | tail -n1
}

parse_wrk_socket_errors() {
  local wrk_log="$1"
  awk -F': ' '/^Socket errors:/ {print $2}' "${wrk_log}" | tail -n1
}

parse_wrk_non2xx() {
  local wrk_log="$1"
  awk '/^Non-2xx or 3xx responses:/ {print $NF}' "${wrk_log}" | tail -n1
}

parse_pidstat_avg_usr_sys() {
  local pidstat_log="$1"
  local pid_csv="$2"  # comma-separated list

  if [[ ! -f "${pidstat_log}" ]]; then
    echo "N/A"
    return
  fi

  # Parse pidstat by locating column indices from the header line that contains
  # PID, %usr and %system. Header format varies across locales and options.
  # We compute a simple average across matching PID data lines.
  awk -v pid_csv="${pid_csv}" '
    BEGIN {
      pid_i = usr_i = sys_i = 0;
      split(pid_csv, pids, ",");
      for (i in pids) want[pids[i]] = 1;
      n = 0;
      usr_sum = 0;
      sys_sum = 0;
    }
    {
      # detect header line (may start with time)
      has_pid = has_usr = has_sys = 0;
      for (i=1; i<=NF; i++) {
        if ($i == "PID") has_pid = 1;
        else if ($i == "%usr") has_usr = 1;
        else if ($i == "%system") has_sys = 1;
      }
      if (has_pid && has_usr && has_sys) {
        for (i=1; i<=NF; i++) {
          if ($i == "PID") pid_i = i;
          else if ($i == "%usr") usr_i = i;
          else if ($i == "%system") sys_i = i;
        }
        next;
      }

      if (pid_i == 0 || usr_i == 0 || sys_i == 0) next;

      pid = $(pid_i);
      if (pid !~ /^[0-9]+$/) next;
      if (!(pid in want)) next;

      usr_sum += ($(usr_i) + 0);
      sys_sum += ($(sys_i) + 0);
      n++;
    }
    END {
      if (n <= 0) { print "N/A"; exit 0; }
      printf("usr=%.2f%% sys=%.2f%% lines=%d", usr_sum/n, sys_sum/n, n);
    }' "${pidstat_log}"
}

to_ms() {
  # Convert wrk latency tokens like 650us/6.95ms/0.12s to milliseconds (float)
  local v="$1"
  if [[ -z "${v}" || "${v}" == "N/A" ]]; then
    echo "N/A"
    return
  fi
  awk -v x="${v}" 'BEGIN {
    n = x;
    sub(/[a-zA-Z%]+$/, "", n);
    u = x;
    sub(/^[0-9.]+/, "", u);
    if (u == "us") printf("%.6f", n/1000.0);
    else if (u == "ms") printf("%.6f", n);
    else if (u == "s") printf("%.6f", n*1000.0);
    else printf("%.6f", n);
  }'
}

stat_avg() {
  # Average numeric list (space-separated). Prints N/A if empty.
  local list="$1"
  if [[ -z "${list}" ]]; then echo "N/A"; return; fi
  awk '{for(i=1;i<=NF;i++){sum+=$i; n++}} END{if(n==0)print "N/A"; else printf("%.4f", sum/n)}' <<<"${list}"
}

stat_median() {
  # Median numeric list (space-separated). Prints N/A if empty.
  local list="$1"
  if [[ -z "${list}" ]]; then echo "N/A"; return; fi
  # shellcheck disable=SC2001
  local sorted
  sorted="$(tr ' ' '\n' <<<"${list}" | grep -E '^[0-9]+' | sort -n)"
  if [[ -z "${sorted}" ]]; then echo "N/A"; return; fi
  local count
  count="$(wc -l <<<"${sorted}" | awk '{print $1}')"
  local mid=$(( (count + 1) / 2 ))
  if (( count % 2 == 1 )); then
    sed -n "${mid}p" <<<"${sorted}"
  else
    local a b
    a="$(sed -n "$((count/2))p" <<<"${sorted}")"
    b="$(sed -n "$((count/2+1))p" <<<"${sorted}")"
    awk -v a="${a}" -v b="${b}" 'BEGIN{printf("%.4f", (a+b)/2.0)}'
  fi
}

extract_request_cache_stats() {
  local stderr_log="$1"
  grep -E "request_cache: " "${stderr_log}" \
    | awk '
      {
        n++;
        for (i=1; i<=NF; i++) {
          split($i, a, "=");
          if (a[1]=="get") get+=a[2];
          else if (a[1]=="hit") hit+=a[2];
          else if (a[1]=="malloc") m+=a[2];
          else if (a[1]=="put") put+=a[2];
          else if (a[1]=="free") fr+=a[2];
          else if (a[1]=="free_now") { if (a[2] > free_now_max) free_now_max=a[2]; }
          else if (a[1]=="free_max") { if (a[2] > free_max_max) free_max_max=a[2]; }
          else if (a[1]=="max_cap") cap=a[2];
        }
      }
      END {
        if (n==0) { print "N/A"; exit 0; }
        printf("workers=%d get=%d hit=%d malloc=%d put=%d free=%d free_now_max=%d free_max_max=%d max_cap=%s", n, get, hit, m, put, fr, free_now_max, free_max_max, cap);
      }'
}

configure_and_build() {
  local build_dir="$1"
  local extra_cflags="$2"

  mkdir -p "${build_dir}"
  # Always re-configure to ensure flags are applied.
  if [[ -n "${extra_cflags}" ]]; then
    cmake -S "${ROOT_DIR}" -B "${build_dir}" -DCMAKE_C_FLAGS="${extra_cflags}"
  else
    cmake -S "${ROOT_DIR}" -B "${build_dir}"
  fi
  cmake --build "${build_dir}"
}

run_one() {
  local name="$1"
  local build_dir="$2"
  local extra_cflags="$3"

  local stdout_log="/tmp/zaver_stdout_${name}.log"
  local stderr_log="/tmp/zaver_stderr_${name}.log"
  local wrk_log="/tmp/zaver_wrk_${name}.log"
  local pidstat_log="/tmp/zaver_pidstat_${name}.log"

  step "Build (${name})"
  configure_and_build "${build_dir}" "${extra_cflags}"

  step "Start server (${name}: ${build_dir}/zaver -c ${CONF_FILE})"
  "${build_dir}/zaver" -c "${CONF_FILE}" >"${stdout_log}" 2>"${stderr_log}" &
  SERVER_PID=$!
  local server_pid="${SERVER_PID}"

  # Worker PID discovery (master/worker model): monitor workers, not just master.
  local pid_list_csv="${server_pid}"
  if command -v pgrep >/dev/null 2>&1; then
    # Give workers a moment to fork.
    sleep 0.05
    while read -r pid; do
      [[ -z "${pid}" ]] && continue
      pid_list_csv+="${pid_list_csv:+,}${pid}"
    done < <(pgrep -P "${server_pid}" 2>/dev/null || true)
  fi

  step "Wait for server to become ready (${name}): ${URL_BASE}/"
  for i in {1..50}; do
    if curl -fsS "${URL_BASE}/" >/dev/null 2>&1; then
      break
    fi
    sleep 0.1
    if ! kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
      echo "Server exited early (${name}). stderr:" >&2
      tail -n 80 "${stderr_log}" >&2 || true
      exit 1
    fi
  done

  step "Correctness (${name}): sha256 local vs downloaded"
  LOCAL_SHA="$(sha256sum "${BIG_PATH}" | awk '{print $1}')"
  TMP_DL="/tmp/${BIG_NAME}.${name}"
  rm -f "${TMP_DL}"
  curl -fsS "${URL_BASE}/${BIG_NAME}" -o "${TMP_DL}"
  REMOTE_SHA="$(sha256sum "${TMP_DL}" | awk '{print $1}')"
  echo "local : ${LOCAL_SHA}"
  echo "remote: ${REMOTE_SHA}"
  if [[ "${LOCAL_SHA}" != "${REMOTE_SHA}" ]]; then
    echo "SHA mismatch (${name})! Likely partial write/duplication bug in EPOLLOUT continuation." >&2
    tail -n 120 "${stderr_log}" >&2 || true
    exit 2
  fi

  step "Optional (${name}): pidstat sys% sampling (if available)"
  PIDSTAT_PID=""
  if command -v pidstat >/dev/null 2>&1; then
    pidstat -u -p "${pid_list_csv}" 1 > "${pidstat_log}" &
    PIDSTAT_PID=$!
    echo "pidstat -> ${pidstat_log}"
  else
    echo "pidstat not found; skipping. (Install: sudo apt-get install sysstat)"
  fi

  step "wrk (${name}): ${THREADS} threads, ${CONNS} conns, ${DURATION} (path=${WRK_PATH})"
  if ! command -v wrk >/dev/null 2>&1; then
    echo "wrk not found. Install e.g.: sudo apt-get install wrk" >&2
    exit 3
  fi
  wrk_args=( -t"${THREADS}" -c"${CONNS}" -d"${DURATION}" )
  if [[ "${WRK_CONN_CLOSE}" == "1" ]]; then
    wrk_args+=( -H "Connection: close" )
  fi
  if [[ "${WRK_LATENCY}" == "1" ]]; then
    wrk_args+=( --latency )
  fi
  wrk "${wrk_args[@]}" "${URL_BASE}${WRK_PATH}" | tee "${wrk_log}"

  step "Stop server (${name})"
  # Send SIGINT to mimic Ctrl+C so workers run their exit paths (cache stats dump).
  kill -INT "${SERVER_PID}" >/dev/null 2>&1 || true
  wait "${SERVER_PID}" >/dev/null 2>&1 || true
  SERVER_PID=""

  if [[ -n "${PIDSTAT_PID:-}" ]]; then
    kill "${PIDSTAT_PID}" >/dev/null 2>&1 || true
    wait "${PIDSTAT_PID}" >/dev/null 2>&1 || true
    PIDSTAT_PID=""
  fi

  # Give worker logs a moment to flush
  sleep 0.2

  local reqps transfer cache_stats latency_line p50 p90 p99 socket_errors non2xx pidstat_avg
  reqps="$(parse_wrk_reqps "${wrk_log}")"
  transfer="$(parse_wrk_transfer "${wrk_log}")"
  cache_stats="$(extract_request_cache_stats "${stderr_log}")"
  latency_line="$(parse_wrk_latency_line "${wrk_log}")"
  if [[ "${WRK_LATENCY}" == "1" ]]; then
    p50="$(parse_wrk_percentile "${wrk_log}" 50)"
    p90="$(parse_wrk_percentile "${wrk_log}" 90)"
    p99="$(parse_wrk_percentile "${wrk_log}" 99)"
  else
    p50="N/A"; p90="N/A"; p99="N/A"
  fi
  socket_errors="$(parse_wrk_socket_errors "${wrk_log}")"
  non2xx="$(parse_wrk_non2xx "${wrk_log}")"
  pidstat_avg="$(parse_pidstat_avg_usr_sys "${pidstat_log}" "${pid_list_csv}")"

  echo
  echo "---- Summary (${name}) ----"
  echo "Requests/sec : ${reqps:-N/A}"
  echo "Transfer/sec : ${transfer:-N/A}"
  echo "Latency      : ${latency_line:-N/A} (avg stdev max)"
  echo "Percentiles  : p50=${p50:-N/A} p90=${p90:-N/A} p99=${p99:-N/A}"
  echo "Socket errors: ${socket_errors:-none}"
  echo "Non-2xx/3xx  : ${non2xx:-0}"
  echo "pidstat avg  : ${pidstat_avg:-N/A}"
  echo "Cache stats  : ${cache_stats:-N/A}"
  echo "Logs         : ${stdout_log} ${stderr_log} ${wrk_log} (${pidstat_log} if enabled)"
  echo "--------------------------"

  # Export for caller
  LAST_REQPS="${reqps}";
  LAST_TRANSFER="${transfer}";
  LAST_CACHE_STATS="${cache_stats}";
  LAST_P99="${p99}";
  LAST_PIDSTAT_AVG="${pidstat_avg}";

  # Numeric fields for aggregation
  LAST_P99_MS="$(to_ms "${p99}")";
  if [[ "${pidstat_avg}" == "N/A" ]]; then
    LAST_SYS_PCT="N/A"
  else
    LAST_SYS_PCT="$(sed -n 's/.*sys=\([0-9.][0-9.]*\)%.*/\1/p' <<<"${pidstat_avg}" | tail -n1)"
    if [[ -z "${LAST_SYS_PCT}" ]]; then
      LAST_SYS_PCT="N/A"
    fi
  fi
}

step "Ensure docroot exists: ${DOCROOT_ABS}"
mkdir -p "${DOCROOT_ABS}"

step "Ensure index.html exists for readiness check"
if [[ ! -f "${DOCROOT_ABS}/index.html" ]]; then
  echo "<html><body>ok</body></html>" > "${DOCROOT_ABS}/index.html"
fi

step "Generate big file (${BIG_PATH}) if missing"
if [[ ! -f "${BIG_PATH}" ]]; then
  dd if=/dev/zero of="${BIG_PATH}" bs=1M count=64 status=progress
fi

step "Generate small file (${SMALL_PATH}) if missing"
if [[ ! -f "${SMALL_PATH}" ]]; then
  dd if=/dev/zero of="${SMALL_PATH}" bs=4K count=1 status=none
fi

if [[ "${COMPARE}" == "1" ]]; then
  step "Compare mode: cache ON vs cache OFF (ZV_REQUEST_FREELIST_MAX=0)"

  # For request freelist testing, keep-alive can hide reuse (few accept/close cycles).
  # Default to forcing connection close in compare mode unless user overrides.
  if [[ "${WRK_CONN_CLOSE}" == "0" ]]; then
    WRK_CONN_CLOSE=1
  fi

  # Percentiles are only printed by wrk when --latency is enabled.
  if [[ "${WRK_LATENCY}" == "0" ]]; then
    WRK_LATENCY=1
  fi

  cache_reqps_list=""; cache_p99ms_list=""; cache_sys_list="";
  nocache_reqps_list=""; nocache_p99ms_list=""; nocache_sys_list="";
  STATS_CACHE=""; STATS_NOCACHE="";

  for ((i=1; i<=REPEAT; i++)); do
    echo
    echo "## Iteration ${i}/${REPEAT} (cache)"
    run_one "cache_${i}" "${BUILD_DIR_CACHE}" ""
    STATS_CACHE="${LAST_CACHE_STATS}"
    if [[ "${LAST_REQPS}" != "N/A" ]]; then cache_reqps_list+=" ${LAST_REQPS}"; fi
    if [[ "${LAST_P99_MS}" != "N/A" ]]; then cache_p99ms_list+=" ${LAST_P99_MS}"; fi
    if [[ "${LAST_SYS_PCT}" != "N/A" ]]; then cache_sys_list+=" ${LAST_SYS_PCT}"; fi

    echo
    echo "## Iteration ${i}/${REPEAT} (nocache)"
    run_one "nocache_${i}" "${BUILD_DIR_NOCACHE}" "-DZV_REQUEST_FREELIST_MAX=0"
    STATS_NOCACHE="${LAST_CACHE_STATS}"
    if [[ "${LAST_REQPS}" != "N/A" ]]; then nocache_reqps_list+=" ${LAST_REQPS}"; fi
    if [[ "${LAST_P99_MS}" != "N/A" ]]; then nocache_p99ms_list+=" ${LAST_P99_MS}"; fi
    if [[ "${LAST_SYS_PCT}" != "N/A" ]]; then nocache_sys_list+=" ${LAST_SYS_PCT}"; fi
  done

  REQPS_CACHE_AVG="$(stat_avg "${cache_reqps_list}")"
  REQPS_CACHE_MED="$(stat_median "${cache_reqps_list}")"
  P99_CACHE_AVG="$(stat_avg "${cache_p99ms_list}")"
  P99_CACHE_MED="$(stat_median "${cache_p99ms_list}")"
  SYS_CACHE_AVG="$(stat_avg "${cache_sys_list}")"
  SYS_CACHE_MED="$(stat_median "${cache_sys_list}")"

  REQPS_NOCACHE_AVG="$(stat_avg "${nocache_reqps_list}")"
  REQPS_NOCACHE_MED="$(stat_median "${nocache_reqps_list}")"
  P99_NOCACHE_AVG="$(stat_avg "${nocache_p99ms_list}")"
  P99_NOCACHE_MED="$(stat_median "${nocache_p99ms_list}")"
  SYS_NOCACHE_AVG="$(stat_avg "${nocache_sys_list}")"
  SYS_NOCACHE_MED="$(stat_median "${nocache_sys_list}")"

  echo
  echo "==== Final Comparison ===="
  echo "repeat=${REPEAT} threads=${THREADS} conns=${CONNS} duration=${DURATION} path=${WRK_PATH} conn_close=${WRK_CONN_CLOSE}"
  echo "cache   : req/s avg=${REQPS_CACHE_AVG} med=${REQPS_CACHE_MED} | p99(ms) avg=${P99_CACHE_AVG} med=${P99_CACHE_MED} | sys% avg=${SYS_CACHE_AVG} med=${SYS_CACHE_MED}"
  echo "nocache : req/s avg=${REQPS_NOCACHE_AVG} med=${REQPS_NOCACHE_MED} | p99(ms) avg=${P99_NOCACHE_AVG} med=${P99_NOCACHE_MED} | sys% avg=${SYS_NOCACHE_AVG} med=${SYS_NOCACHE_MED}"
  echo "cache stats   : ${STATS_CACHE:-N/A}"
  echo "nocache stats : ${STATS_NOCACHE:-N/A}"
  echo "=========================="
else
  step "Single-run mode (compatible): build+run using BUILD_DIR=${BUILD_DIR}"
  run_one "single" "${BUILD_DIR}" ""
fi
