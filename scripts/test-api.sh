#!/usr/bin/env bash

set -euo pipefail

BASE_URL="${SERVER_API_URL:-http://127.0.0.1:7878}"
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
request "server with default provider" 200 "$BASE_URL/api/v1/server"
request "metrics with default provider" 200 "$BASE_URL/api/v1/server/metrics"
request "players with default provider" 200 "$BASE_URL/api/v1/players?limit=10"
request "groups with default provider" 200 "$BASE_URL/api/v1/groups"
request "instances with default provider" 200 "$BASE_URL/api/v1/instances"
request "module namespace reaches routing" 404 "$BASE_URL/api/v1/mod/unregistered/route"
if [[ "$PLAYERBOTS_ENABLED" == "1" ]]; then
    request "bots with default provider" 200 "$BASE_URL/api/v1/bots"
else
    request "bots unsupported" 501 "$BASE_URL/api/v1/bots"
fi

if [[ -n "$PLAYER_GUID" ]]; then
    request "player details with default provider" 200 "$BASE_URL/api/v1/players/$PLAYER_GUID"
else
    echo "SKIP: player details; set SERVER_API_PLAYER_GUID to test a known online player"
fi

echo "All API checks passed."
