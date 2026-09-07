#!/usr/bin/env bash

# Manual curl checks for mod-server-api.
# Default mode matches a trusted localhost deployment with Auth.Enable = 0.
# For authenticated mode set SERVER_API_KEY and use AUTH_HEADER below.

set -u

BASE_URL="${SERVER_API_URL:-http://127.0.0.1:7878}"
API_KEY="${SERVER_API_KEY:-}"
AUTH_HEADER="Authorization: Bearer ${API_KEY}"

echo "===== 1. Health check: liveness ====="
curl -i "${BASE_URL}/health"

echo "===== 2. Readiness check ====="
curl -i "${BASE_URL}/ready"

echo "===== 3. Server runtime snapshot ====="
curl -i "${BASE_URL}/api/v1/server"

echo "===== 4. Server metrics ====="
curl -i "${BASE_URL}/api/v1/server/metrics"

echo "===== 5. Online players, first 10 ====="
curl -i "${BASE_URL}/api/v1/players?limit=10"

echo "===== 6. Online players filtered by name/map ====="
curl -i "${BASE_URL}/api/v1/players?name=Papas&mapId=0&limit=25"

echo "===== 7. Player details; replace 123 with an online GUID ====="
curl -i "${BASE_URL}/api/v1/players/123"

echo "===== 8. Groups list ====="
curl -i "${BASE_URL}/api/v1/groups"

echo "===== 9. Group details; replace 42 with an existing group ID ====="
curl -i "${BASE_URL}/api/v1/groups/42"

echo "===== 10. Dungeon/raid instances list ====="
curl -i "${BASE_URL}/api/v1/instances"

echo "===== 11. Instance details; replace 1 with an existing instance ID ====="
curl -i "${BASE_URL}/api/v1/instances/1"

echo "===== 12. Playerbots list ====="
curl -i "${BASE_URL}/api/v1/bots"

echo "===== 13. Dungeon Clear catalog; expected 200 with mod-dungeon-clear or 501 without it ====="
curl -i "${BASE_URL}/api/v1/dungeon-clear/dungeons"

echo "===== 14. Dungeon Clear active runs; expected 200 with mod-dungeon-clear or 501 without it ====="
curl -i "${BASE_URL}/api/v1/dungeon-clear/runs"

echo "===== 15. Invalid player map filter; expected 400 ====="
curl -i "${BASE_URL}/api/v1/players?mapId=invalid"

echo "===== 16. Account details; replace 1 with an account ID or name ====="
curl -i "${BASE_URL}/api/v1/accounts/1"

echo "===== 17. Account characters; replace 1 with an account ID or name ====="
curl -i "${BASE_URL}/api/v1/accounts/1/characters"

echo "===== 18. Invalid player limit; expected 400 ====="
curl -i "${BASE_URL}/api/v1/players?limit=0"

echo "===== 17. Invalid group ID; expected 400 ====="
curl -i "${BASE_URL}/api/v1/groups/not-a-number"

echo "===== 18. Missing instance; expected 404 ====="
curl -i "${BASE_URL}/api/v1/instances/999999999"

echo "===== 19. Invalid dungeon-clear start; safe, no state change, expected 400 ====="
curl -i -X POST "${BASE_URL}/api/v1/dungeon-clear/runs/start"

echo "===== 20. Invalid kick command; safe, no state change, expected 400 ====="
curl -i -X POST "${BASE_URL}/api/v1/players/not-a-guid/kick"

echo "===== 21. Invalid teleport command; safe, no state change, expected 400 ====="
curl -i -X POST \
  "${BASE_URL}/api/v1/players/1/teleport?mapId=bad&x=0&y=0&z=0&orientation=0"

if [[ -n "${API_KEY}" ]]; then
    echo "===== 22. Authenticated server request ====="
    curl -i -H "${AUTH_HEADER}" "${BASE_URL}/api/v1/server"

    echo "===== 23. Authenticated WebSocket upgrade request ====="
    curl -i --http1.1 --max-time 3 \
      -H "Connection: Upgrade" \
      -H "Upgrade: websocket" \
      -H "Sec-WebSocket-Version: 13" \
      -H "Sec-WebSocket-Key: SGVsbG9XZWJTb2NrZXQ=" \
      -H "${AUTH_HEADER}" \
      "${BASE_URL}/ws/v1/events"
else
    echo "===== 19. Authenticated checks skipped ====="
    echo "Set SERVER_API_KEY when ServerApi.Auth.Enable = 1."
fi

echo "===== WebSocket functional test ====="
echo "Use a WebSocket client to send:"
echo '{"type":"subscribe","events":["player.position","combat.snapshot"]}'
echo '{"type":"ping"}'
