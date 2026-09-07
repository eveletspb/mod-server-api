#!/usr/bin/env bash

set -euo pipefail

BASE_URL="${SERVER_API_URL:-http://127.0.0.1:7878}"
API_KEY="${SERVER_API_KEY:-}"
PLAYER_GUID="${SERVER_API_PLAYER_GUID:-}"
PLAYERBOTS_ENABLED="${SERVER_API_PLAYERBOTS:-0}"
TMP_BODY="$(mktemp)"
trap 'rm -f "$TMP_BODY"' EXIT

request() {
    local name="$1"
    local expected_status="$2"
    shift 2

    local status
    if ! status="$(curl --silent --show-error --connect-timeout 2 --max-time 5 \
        --output "$TMP_BODY" --write-out '%{http_code}' "$@")"; then
        echo "FAIL: $name (API is unreachable)"
        exit 1
    fi

    if [[ "$status" != "$expected_status" ]]; then
        echo "FAIL: $name (expected HTTP $expected_status, got HTTP $status)"
        sed -n '1,3p' "$TMP_BODY"
        exit 1
    fi

    echo "PASS: $name (HTTP $status)"
}

echo "Testing mod-server-api at $BASE_URL"

request "health" 200 "$BASE_URL/health"
request "readiness" 200 "$BASE_URL/ready"
request "server without token" 401 "$BASE_URL/api/v1/server"
request "metrics without token" 401 "$BASE_URL/api/v1/server/metrics"
request "players without token" 401 "$BASE_URL/api/v1/players"
request "groups without token" 401 "$BASE_URL/api/v1/groups"
request "instances without token" 401 "$BASE_URL/api/v1/instances"

if [[ -z "$API_KEY" ]]; then
    echo "SKIP: authorized API checks; set SERVER_API_KEY to run them"
    exit 0
fi

AUTH_HEADER="Authorization: Bearer $API_KEY"
request "server with token" 200 -H "$AUTH_HEADER" "$BASE_URL/api/v1/server"
request "metrics with token" 200 -H "$AUTH_HEADER" "$BASE_URL/api/v1/server/metrics"
request "players with token" 200 -H "$AUTH_HEADER" "$BASE_URL/api/v1/players?limit=10"
request "groups with token" 200 -H "$AUTH_HEADER" "$BASE_URL/api/v1/groups"
request "instances with token" 200 -H "$AUTH_HEADER" "$BASE_URL/api/v1/instances"
if [[ "$PLAYERBOTS_ENABLED" == "1" ]]; then
    request "bots with token" 200 -H "$AUTH_HEADER" "$BASE_URL/api/v1/bots"
else
    request "bots unsupported" 501 -H "$AUTH_HEADER" "$BASE_URL/api/v1/bots"
fi

if [[ -n "$PLAYER_GUID" ]]; then
    request "player details with token" 200 -H "$AUTH_HEADER" "$BASE_URL/api/v1/players/$PLAYER_GUID"
else
    echo "SKIP: player details; set SERVER_API_PLAYER_GUID to test a known online player"
fi

echo "All API checks passed."
