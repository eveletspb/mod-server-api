#!/usr/bin/env bash

set -euo pipefail

BASE_URL="${SERVER_API_URL:-http://127.0.0.1:7878}"
PLAYER_GUID="${SERVER_API_PLAYER_GUID:-}"
PLAYERBOTS_ENABLED="${SERVER_API_PLAYERBOTS:-0}"
BODY_FILE="$(mktemp)"
trap 'rm -f "$BODY_FILE"' EXIT

assert_response() {
    local name="$1"
    local expected_status="$2"
    local expected_body="$3"
    shift 3

    local status
    if ! status="$(curl --silent --show-error --connect-timeout 2 --max-time 5 \
        --output "$BODY_FILE" --write-out '%{http_code}' "$@")"; then
        echo "FAIL: $name (API is unreachable)" >&2
        exit 1
    fi

    if [[ "$status" != "$expected_status" ]]; then
        echo "FAIL: $name (expected HTTP $expected_status, got HTTP $status)" >&2
        cat "$BODY_FILE" >&2
        exit 1
    fi

    if [[ -n "$expected_body" ]] && ! grep -Fq "$expected_body" "$BODY_FILE"; then
        echo "FAIL: $name (response does not contain: $expected_body)" >&2
        cat "$BODY_FILE" >&2
        exit 1
    fi

    echo "PASS: $name"
}

assert_response "health" 200 '"status":"ok"' \
    "$BASE_URL/health"
assert_response "ready" 200 '"status":"ready"' \
    "$BASE_URL/ready"
assert_response "health rejects POST" 405 'METHOD_NOT_ALLOWED' \
    -X POST "$BASE_URL/health"
assert_response "server" 200 'playersOnline' \
    "$BASE_URL/api/v1/server"
assert_response "metrics" 200 'activeMaps' \
    "$BASE_URL/api/v1/server/metrics"
assert_response "players list" 200 '"data"' \
    "$BASE_URL/api/v1/players?limit=10"
assert_response "groups list" 200 '"data"' \
    "$BASE_URL/api/v1/groups"
assert_response "instances list" 200 '"data"' \
    "$BASE_URL/api/v1/instances"
assert_response "module namespace reaches routing" 404 'NOT_FOUND' \
    "$BASE_URL/api/v1/mod/unregistered/route"
if [[ "$PLAYERBOTS_ENABLED" == "1" ]]; then
    assert_response "bots endpoint" 200 '"data"' \
        "$BASE_URL/api/v1/bots"
else
    assert_response "bots unsupported" 501 'NOT_SUPPORTED' \
        "$BASE_URL/api/v1/bots"
fi
assert_response "invalid map filter" 400 'INVALID_MAP_ID' \
    "$BASE_URL/api/v1/players?mapId=invalid"
assert_response "invalid limit" 400 'INVALID_LIMIT' \
    "$BASE_URL/api/v1/players?limit=0"
assert_response "invalid group id" 400 'INVALID_GROUP_ID' \
    "$BASE_URL/api/v1/groups/not-a-number"
assert_response "missing instance" 404 'INSTANCE_NOT_FOUND' \
    "$BASE_URL/api/v1/instances/999999999"
assert_response "account API removed" 404 'NOT_FOUND' \
    "$BASE_URL/api/v1/accounts/1"

if [[ -n "$PLAYER_GUID" ]]; then
    assert_response "player details" 200 '"position"' \
        "$BASE_URL/api/v1/players/$PLAYER_GUID"
else
    echo "SKIP: player details (set SERVER_API_PLAYER_GUID)"
fi

echo "API contract tests passed."
