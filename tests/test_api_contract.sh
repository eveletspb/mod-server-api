#!/usr/bin/env bash

set -euo pipefail

BASE_URL="${SERVER_API_URL:-http://127.0.0.1:7878}"
API_KEY="${SERVER_API_KEY:-}"
PLAYER_GUID="${SERVER_API_PLAYER_GUID:-}"
ACCOUNT_ID="${SERVER_API_ACCOUNT_ID:-}"
PLAYERBOTS_ENABLED="${SERVER_API_PLAYERBOTS:-0}"
DUNGEON_CLEAR_ENABLED="${SERVER_API_DUNGEON_CLEAR:-0}"
BODY_FILE="$(mktemp)"
trap 'rm -f "$BODY_FILE"' EXIT

if [[ -z "$API_KEY" ]]; then
    echo "SERVER_API_KEY is required" >&2
    exit 2
fi

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

AUTH_HEADER="Authorization: Bearer $API_KEY"

assert_response "health" 200 '"status":"ok"' \
    "$BASE_URL/health"
assert_response "ready" 200 '"status":"ready"' \
    "$BASE_URL/ready"
assert_response "missing auth" 401 'UNAUTHORIZED' \
    "$BASE_URL/api/v1/server"
assert_response "invalid auth" 401 'UNAUTHORIZED' \
    -H 'Authorization: Bearer invalid' "$BASE_URL/api/v1/server"
assert_response "server" 200 'playersOnline' \
    -H "$AUTH_HEADER" "$BASE_URL/api/v1/server"
assert_response "metrics" 200 'activeMaps' \
    -H "$AUTH_HEADER" "$BASE_URL/api/v1/server/metrics"
assert_response "players list" 200 '"data"' \
    -H "$AUTH_HEADER" "$BASE_URL/api/v1/players?limit=10"
assert_response "groups list" 200 '"data"' \
    -H "$AUTH_HEADER" "$BASE_URL/api/v1/groups"
assert_response "instances list" 200 '"data"' \
    -H "$AUTH_HEADER" "$BASE_URL/api/v1/instances"
if [[ "$DUNGEON_CLEAR_ENABLED" == "1" ]]; then
    assert_response "dungeon-clear catalog" 200 '"data"' \
        -H "$AUTH_HEADER" "$BASE_URL/api/v1/dungeon-clear/dungeons"
    assert_response "dungeon-clear runs" 200 '"runs"' \
        -H "$AUTH_HEADER" "$BASE_URL/api/v1/dungeon-clear/runs"
else
    assert_response "dungeon-clear unsupported" 501 'NOT_SUPPORTED' \
        -H "$AUTH_HEADER" "$BASE_URL/api/v1/dungeon-clear/dungeons"
fi
if [[ "$PLAYERBOTS_ENABLED" == "1" ]]; then
    assert_response "bots endpoint" 200 '"data"' \
        -H "$AUTH_HEADER" "$BASE_URL/api/v1/bots"
else
    assert_response "bots unsupported" 501 'NOT_SUPPORTED' \
        -H "$AUTH_HEADER" "$BASE_URL/api/v1/bots"
fi
assert_response "invalid map filter" 400 'INVALID_MAP_ID' \
    -H "$AUTH_HEADER" "$BASE_URL/api/v1/players?mapId=invalid"
assert_response "invalid limit" 400 'INVALID_LIMIT' \
    -H "$AUTH_HEADER" "$BASE_URL/api/v1/players?limit=0"
assert_response "invalid group id" 400 'INVALID_GROUP_ID' \
    -H "$AUTH_HEADER" "$BASE_URL/api/v1/groups/not-a-number"
assert_response "missing instance" 404 'INSTANCE_NOT_FOUND' \
    -H "$AUTH_HEADER" "$BASE_URL/api/v1/instances/999999999"

if [[ -n "$PLAYER_GUID" ]]; then
    assert_response "player details" 200 '"position"' \
        -H "$AUTH_HEADER" "$BASE_URL/api/v1/players/$PLAYER_GUID"
else
    echo "SKIP: player details (set SERVER_API_PLAYER_GUID)"
fi

if [[ -n "$ACCOUNT_ID" ]]; then
    assert_response "account details" 200 '"username"' \
        -H "$AUTH_HEADER" "$BASE_URL/api/v1/accounts/$ACCOUNT_ID"
    assert_response "account characters" 200 '"data"' \
        -H "$AUTH_HEADER" "$BASE_URL/api/v1/accounts/$ACCOUNT_ID/characters"
else
    echo "SKIP: account checks (set SERVER_API_ACCOUNT_ID)"
fi

echo "API contract tests passed."
