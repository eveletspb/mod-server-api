#!/usr/bin/env bash

# Read-only REST load smoke test.
# Examples:
#   ./tests/load-api.sh
#   SERVER_API_REQUESTS=1000 SERVER_API_CONCURRENCY=20 ./tests/load-api.sh

set -euo pipefail

BASE_URL="${SERVER_API_URL:-http://127.0.0.1:7878}"
ENDPOINT="${SERVER_API_LOAD_ENDPOINT:-/health}"
REQUESTS="${SERVER_API_REQUESTS:-100}"
CONCURRENCY="${SERVER_API_CONCURRENCY:-10}"
API_KEY="${SERVER_API_KEY:-}"

if ! [[ "$REQUESTS" =~ ^[1-9][0-9]*$ && "$CONCURRENCY" =~ ^[1-9][0-9]*$ ]]; then
    echo "REQUESTS and CONCURRENCY must be positive integers" >&2
    exit 2
fi

if ! [[ "$ENDPOINT" = /* ]]; then
    echo "SERVER_API_LOAD_ENDPOINT must start with /" >&2
    exit 2
fi

URL="${BASE_URL}${ENDPOINT}"
echo "Load test: ${REQUESTS} requests, concurrency ${CONCURRENCY}, URL ${URL}"

failures=0
export URL
export API_KEY

run_request() {
    local code
    if [[ -n "$API_KEY" ]]; then
        code="$(curl --silent --show-error --connect-timeout 2 --max-time 5 \
            -H "Authorization: Bearer ${API_KEY}" -o /dev/null -w '%{http_code}' "$URL" || true)"
    else
        code="$(curl --silent --show-error --connect-timeout 2 --max-time 5 \
            -o /dev/null -w '%{http_code}' "$URL" || true)"
    fi
    [[ "$code" == "200" ]] || { echo "HTTP ${code}"; return 1; }
}

export -f run_request

if command -v xargs >/dev/null 2>&1; then
    failures="$(seq "$REQUESTS" | xargs -P "$CONCURRENCY" -I {} bash -c 'run_request' 2>&1 | wc -l | tr -d ' ')"
else
    echo "xargs is required" >&2
    exit 2
fi

if [[ "$failures" != "0" ]]; then
    echo "FAIL: ${failures} requests did not return HTTP 200" >&2
    exit 1
fi

echo "PASS: all ${REQUESTS} requests returned HTTP 200"
