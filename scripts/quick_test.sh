#!/usr/bin/env bash
set -euo pipefail

# Quick functional tests for Zaver (no wrk required)
# - Builds
# - Starts server
# - Checks a few URLs and keep-alive

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CONF_FILE="${CONF_FILE:-${ROOT_DIR}/zaver.conf}"
BUILD_DIR="${BUILD_DIR:-${ROOT_DIR}/build}"

PORT="${PORT:-}"
DOCROOT="${DOCROOT:-}"

if [[ -z "${PORT}" ]]; then
  PORT="$(awk -F= '$1=="port"{print $2}' "${CONF_FILE}" | tail -n1)"
fi
if [[ -z "${DOCROOT}" ]]; then
  DOCROOT="$(awk -F= '$1=="root"{print $2}' "${CONF_FILE}" | tail -n1)"
fi

DOCROOT_ABS="${ROOT_DIR}/${DOCROOT#./}"
URL_BASE="http://127.0.0.1:${PORT}"

cleanup() {
  if [[ -n "${SERVER_PID:-}" ]]; then
    kill "${SERVER_PID}" >/dev/null 2>&1 || true
    wait "${SERVER_PID}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

cmake --build "${BUILD_DIR}"

mkdir -p "${DOCROOT_ABS}"
if [[ ! -f "${DOCROOT_ABS}/index.html" ]]; then
  echo "<html><body>ok</body></html>" > "${DOCROOT_ABS}/index.html"
fi

"${BUILD_DIR}/zaver" -c "${CONF_FILE}" >/tmp/zaver_stdout.log 2>/tmp/zaver_stderr.log &
SERVER_PID=$!

for i in {1..50}; do
  if curl -fsS "${URL_BASE}/" >/dev/null 2>&1; then
    break
  fi
  sleep 0.1
  if ! kill -0 "${SERVER_PID}" >/dev/null 2>&1; then
    echo "Server exited early. stderr:" >&2
    tail -n 50 /tmp/zaver_stderr.log >&2 || true
    exit 1
  fi
done

echo "GET / (should be 200)"
curl -v "${URL_BASE}/" -o /dev/null

echo "GET /not-exist (should be 404)"
curl -v "${URL_BASE}/not-exist" -o /dev/null || true

echo "Keep-alive: two requests in one curl invocation"
curl --http1.1 -v "${URL_BASE}/" "${URL_BASE}/" -o /dev/null

echo "OK. Logs: /tmp/zaver_stdout.log /tmp/zaver_stderr.log"
