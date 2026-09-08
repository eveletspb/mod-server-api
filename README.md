# mod-server-api

Custom AzerothCore module exposing a small, local-first HTTP API for the
`worldserver` runtime.

Implemented now: startup banner, lifecycle management, public `/health` and
`/ready`, Bearer-protected `/api/v1/server` and `/api/v1/server/metrics`,
online players, groups, active dungeon/raid instances, a world-thread server
snapshot, a bounded in-process EventBus, and Bearer-protected
`/ws/v1/events` with subscriptions and ping/pong. When `mod-dungeon-clear` is
compiled into the same worldserver, the API also exposes its dungeon/raid test
run catalog and world-thread-safe run controls.

## Supported API

Default address:

```text
http://127.0.0.1:7878
```

The listener is disabled by default. With authentication enabled, versioned
HTTP endpoints and WebSocket handshakes require:

```http
Authorization: Bearer <ServerApi.Auth.ApiKey>
```

### Health

These endpoints are public and intended for liveness/readiness probes.

| Method | Endpoint | Response |
|---|---|---|
| `GET` | `/health` | `{"status":"ok"}` |
| `GET` | `/ready` | `{"status":"ready"}` |

### Server

| Method | Endpoint | Auth |
|---|---|---|
| `GET` | `/api/v1/server` | Bearer |
| `GET` | `/api/v1/server/metrics` | Bearer |

`/api/v1/server` returns `realmId`, `serverTime`, `uptimeSeconds`,
`playersOnline` and `botsOnline`.

`/api/v1/server/metrics` returns `activeMaps`, `activeInstances`,
`playersOnline`, `botsOnline`, `eventQueue`, `commandQueue` and
`droppedEvents`.

Example:

```bash
curl -H 'Authorization: Bearer test-secret-key' \
  http://127.0.0.1:7878/api/v1/server

curl -H 'Authorization: Bearer test-secret-key' \
  http://127.0.0.1:7878/api/v1/server/metrics
```

### Players

The current implementation exposes online runtime players from the world
thread snapshot. Offline characters and database-backed pagination are not
included.

| Method | Endpoint | Auth |
|---|---|---|
| `GET` | `/api/v1/players` | Bearer |
| `GET` | `/api/v1/players/{guid}` | Bearer |

Supported list query parameters:

| Parameter | Description |
|---|---|
| `name` | Case-sensitive substring filter |
| `mapId` | Exact map ID filter |
| `limit` | Maximum number of records, capped at 1000; default 100 |

Example:

```bash
curl -H 'Authorization: Bearer test-secret-key' \
  'http://127.0.0.1:7878/api/v1/players?limit=10&mapId=0'

curl -H 'Authorization: Bearer test-secret-key' \
  http://127.0.0.1:7878/api/v1/players/123
```

The list response has the following shape:

```json
{
  "data": [
    {
      "guid": 123,
      "name": "Papas",
      "level": 60,
      "class": 1,
      "race": 2,
      "mapId": 0,
      "zoneId": 12,
      "online": true
    }
  ]
}
```

The detail response additionally contains `health`, `power` and `position`.
`power` uses the player's active class power type (mana, rage, energy or runic
power).

### Groups

| Method | Endpoint | Auth |
|---|---|---|
| `GET` | `/api/v1/groups` | Bearer |
| `GET` | `/api/v1/groups/{id}` | Bearer |

Groups are collected from online players on the world thread. A group with no
online player is not visible in this runtime snapshot. Each item contains
`id`, `leaderGuid`, `raid` and the `members` array of character GUIDs.

### Instances

| Method | Endpoint | Auth |
|---|---|---|
| `GET` | `/api/v1/instances` | Bearer |
| `GET` | `/api/v1/instances/{instanceId}` | Bearer |

The response contains active dungeon and raid maps only. Each item contains
`instanceId`, `mapId`, `difficulty` and the current non-GM `players` count.
Persistent instance-save state and a `startedAt` timestamp are not exposed.

### Bots

| Method | Endpoint | Auth |
|---|---|---|
| `GET` | `/api/v1/bots` | Bearer |

When `mod-playerbots` is compiled into the worldserver, the endpoint returns
online bot players with the same basic fields as the player list. Without
`mod-playerbots`, it returns `501 NOT_SUPPORTED`; the server API module still
builds normally. Bot-specific invite/kick commands remain deferred until the
playerbots integration is connected to the command queue.

### Dungeon Clear integration

These routes are available when both modules are compiled into the same
worldserver. The catalog is taken from `mod-dungeon-clear`'s existing `.dc
test` registry and includes supported dungeons and raids (`mc`, `bwl`, `gruul`).

| Method | Endpoint | Auth | Description |
|---|---|---|---|
| `GET` | `/api/v1/dungeon-clear/dungeons` | Bearer | Supported dungeon and raid tokens |
| `GET` | `/api/v1/dungeon-clear/runs` | Bearer | Active run status in JSON |
| `POST` | `/api/v1/dungeon-clear/runs/start?dungeon=<token>` | Bearer | Queue a new run |
| `POST` | `/api/v1/dungeon-clear/runs/stop?selector=<runId\|token\|all>` | Bearer | Queue cancellation |

Run start accepts optional `heroic=true|false`, `size=2..40`, `level=N` and
`seed=N`. `size=10` or `size=25` is intended for raid runs; omitting it keeps
the normal 5-man composition. Start and stop return `202 Accepted` and execute
on the next world update.

```bash
curl -H "Authorization: Bearer ${API_KEY}" \
  http://127.0.0.1:7878/api/v1/dungeon-clear/dungeons

curl -H "Authorization: Bearer ${API_KEY}" \
  http://127.0.0.1:7878/api/v1/dungeon-clear/runs

curl -i -X POST -H "Authorization: Bearer ${API_KEY}" \
  'http://127.0.0.1:7878/api/v1/dungeon-clear/runs/start?dungeon=uk&heroic=true'

curl -i -X POST -H "Authorization: Bearer ${API_KEY}" \
  'http://127.0.0.1:7878/api/v1/dungeon-clear/runs/start?dungeon=mc&size=10&level=60'

curl -i -X POST -H "Authorization: Bearer ${API_KEY}" \
  'http://127.0.0.1:7878/api/v1/dungeon-clear/runs/stop?selector=all'
```

Without `mod-dungeon-clear`, these routes return `501 NOT_SUPPORTED`.

### Accounts

Account routes are administrative and require the configured Bearer key. The
API never returns passwords, SRP data or session keys. Read operations query
the core's prepared account/character statements; write operations are queued
to the world thread.

| Method | Endpoint | Purpose |
|---|---|---|
| `GET` | `/api/v1/accounts/{id-or-name}` | Account details, status and counters |
| `GET` | `/api/v1/accounts/{id-or-name}/characters` | Account characters |
| `POST` | `/api/v1/accounts/create?username=&password=&email=` | Create account |
| `POST` | `/api/v1/accounts/{id}/password?password=` | Change password |
| `POST` | `/api/v1/accounts/{id}/email?email=` | Change email |
| `POST` | `/api/v1/accounts/{id}/username?username=&password=` | Change username |
| `POST` | `/api/v1/accounts/{id}/lock?value=0\|1` | Lock or unlock |
| `POST` | `/api/v1/accounts/{id}/expansion?value=N` | Set expansion level |
| `POST` | `/api/v1/accounts/{id}/flags?value=N` | Set account flags |
| `POST` | `/api/v1/accounts/{id}/ban?duration=7d&reason=` | Ban account |
| `POST` | `/api/v1/accounts/{id}/unban` | Remove account ban |
| `POST` | `/api/v1/accounts/{id}/mute?duration=1h&reason=` | Mute account |
| `POST` | `/api/v1/accounts/{id}/unmute` | Remove mute |
| `POST` | `/api/v1/accounts/{id}/delete?confirm=DELETE` | Delete account and characters |

All write operations return `202 Accepted`. Account deletion is intentionally
guarded by the exact `confirm=DELETE` parameter.
`lock` accepts only `0` or `1`, expansion accepts the WotLK range `0..2`, and
invalid numeric values or durations are rejected before a command is queued.

### Commands

`POST /api/v1/players/{guid}/kick` queues a player kick for execution on the
world thread. It does not run game-object operations from the HTTP worker.
The normal response is `202 Accepted`:

```bash
curl -i -X POST \
  -H "Authorization: Bearer ${API_KEY}" \
  http://127.0.0.1:7878/api/v1/players/123/kick
```

The command queue is bounded. If it is full, the API returns
`503 COMMAND_QUEUE_FULL`. This command is intentionally not included in the
automatic contract test because it changes server state and can disconnect a
real player.

`POST /api/v1/players/{guid}/teleport` uses the same queue and requires
`mapId`, `x`, `y`, `z` and `orientation` query parameters:

```bash
curl -i -X POST \
  -H "Authorization: Bearer ${API_KEY}" \
  'http://127.0.0.1:7878/api/v1/players/123/teleport?mapId=0&x=-8949.95&y=-132.49&z=83.53&orientation=0'
```

### WebSocket events

Endpoint:

```text
ws://127.0.0.1:7878/ws/v1/events
```

The WebSocket handshake requires the same Bearer header. After connecting,
send a compact JSON subscription message:

```json
{"type":"subscribe","events":["server.*","player.*","group.*","instance.*"]}
```

Supported client messages:

```json
{"type":"subscribe","events":["player.*"]}
{"type":"unsubscribe"}
{"type":"ping"}
```

The server responds with `subscribed`, `unsubscribed` or `pong`. Event messages
use this envelope:

```json
{
  "type": "player.login",
  "timestamp": 1788753200123,
  "version": 1,
  "data": {
    "guid": "123",
    "name": "Papas",
    "level": "60",
    "mapId": "0",
    "zoneId": "12"
  }
}
```

Currently published events:

| Event | Source |
|---|---|
| `server.started` | `ServerApiWorldScript::OnStartup` |
| `server.stopping` | `ServerApiWorldScript::OnShutdown` |
| `player.login` | `PlayerScript::OnPlayerLogin` |
| `player.logout` | `PlayerScript::OnPlayerLogout` |
| `player.death` | `PlayerScript::OnPlayerJustDied` |
| `player.position` | Player map or position changes in the world snapshot |
| `combat.snapshot` | Sampled active-combat state for one player |
| `group.created` | Group appears in the online-player snapshot |
| `group.updated` | Group leader, raid flag or member list changes |
| `group.disbanded` | Group leaves the online-player snapshot |
| `instance.started` | Dungeon/raid instance appears in the snapshot |
| `instance.updated` | Instance difficulty or non-GM player count changes |
| `instance.stopped` | Dungeon/raid instance leaves the snapshot |

Subscriptions use exact event names or a trailing wildcard such as `player.*`.
Each WebSocket has a bounded outgoing queue; an overflowing connection is
closed to protect the API worker.
The total number of WebSocket clients is limited by
`ServerApi.WebSocket.MaxClients` (default 50).

Position sampling uses `ServerApi.PositionUpdates.IntervalMs` (default 1000 ms)
and is performed from the world-thread snapshot. The module does not emit one
event per movement packet.

Combat snapshots use `ServerApi.CombatSnapshot.IntervalMs` (default 2000 ms)
and include `guid`, `mapId`, `victimGuid`, `health`, `maxHealth`, `power` and
`maxPower` in the event data. Damage, healing and threat counters are not
included yet; collecting them requires a dedicated low-overhead combat
aggregator.

### HTTP errors

| Status | Meaning |
|---:|---|
| `400` | Invalid request or command parameters |
| `401` | Missing or invalid Bearer token |
| `404` | Unknown endpoint or player not found |
| `405` | Known endpoint called with an unsupported method |
| `413` | Request exceeds `ServerApi.MaxRequestBytes` |
| `429` | Global HTTP request rate limit exceeded |
| `501` | Optional integration is unavailable |
| `503` | Command queue or WebSocket client limit reached |

### cURL examples

Проверка доступности API без авторизации:

```bash
curl -i http://127.0.0.1:7878/health
curl -i http://127.0.0.1:7878/ready
```

Проверка отказа без Bearer-токена:

```bash
curl -i http://127.0.0.1:7878/api/v1/server
curl -i http://127.0.0.1:7878/api/v1/players
```

Запросы с авторизацией:

```bash
API_KEY='test-secret-key'

curl -i \
  -H "Authorization: Bearer ${API_KEY}" \
  http://127.0.0.1:7878/api/v1/server

curl -i \
  -H "Authorization: Bearer ${API_KEY}" \
  http://127.0.0.1:7878/api/v1/server/metrics

curl -i \
  -H "Authorization: Bearer ${API_KEY}" \
  'http://127.0.0.1:7878/api/v1/players?limit=10'

curl -i \
  -H "Authorization: Bearer ${API_KEY}" \
  'http://127.0.0.1:7878/api/v1/players?name=Papas&mapId=0&limit=25'

curl -i \
  -H "Authorization: Bearer ${API_KEY}" \
  http://127.0.0.1:7878/api/v1/players/123

curl -i \
  -H "Authorization: Bearer ${API_KEY}" \
  http://127.0.0.1:7878/api/v1/groups

curl -i \
  -H "Authorization: Bearer ${API_KEY}" \
  http://127.0.0.1:7878/api/v1/groups/123

curl -i \
  -H "Authorization: Bearer ${API_KEY}" \
  http://127.0.0.1:7878/api/v1/instances

curl -i \
  -H "Authorization: Bearer ${API_KEY}" \
  http://127.0.0.1:7878/api/v1/instances/1

curl -i \
  -H "Authorization: Bearer ${API_KEY}" \
  http://127.0.0.1:7878/api/v1/bots
```

Проверка ошибок фильтров и неизвестного игрока:

```bash
curl -i \
  -H "Authorization: Bearer ${API_KEY}" \
  'http://127.0.0.1:7878/api/v1/players?mapId=invalid'

curl -i \
  -H "Authorization: Bearer ${API_KEY}" \
  'http://127.0.0.1:7878/api/v1/players?limit=0'

curl -i \
  -H "Authorization: Bearer ${API_KEY}" \
  http://127.0.0.1:7878/api/v1/players/999999999

curl -i \
  -H "Authorization: Bearer ${API_KEY}" \
  http://127.0.0.1:7878/api/v1/groups/not-a-number

curl -i \
  -H "Authorization: Bearer ${API_KEY}" \
  http://127.0.0.1:7878/api/v1/instances/999999999
```

## Configuration

```ini
ServerApi.Enable = 1
ServerApi.BindAddress = "127.0.0.1"
ServerApi.Port = 7878
ServerApi.MaxRequestBytes = 1048576
ServerApi.MaxRequestsPerSecond = 1000
ServerApi.PositionUpdates.IntervalMs = 1000
ServerApi.CombatSnapshot.IntervalMs = 2000
ServerApi.WebSocket.Enable = 1
ServerApi.WebSocket.MaxFrameBytes = 1048576
ServerApi.WebSocket.MaxSubscriptions = 100
ServerApi.WebSocket.MaxQueue = 100
ServerApi.WebSocket.MaxClients = 50
ServerApi.Auth.Enable = 1
ServerApi.Auth.ApiKey = "test-secret-key"
```

To run the service without authentication on localhost:

```ini
ServerApi.Enable = 1
ServerApi.BindAddress = "127.0.0.1"
ServerApi.Auth.Enable = 0
ServerApi.Auth.ApiKey = ""
```

With `Auth.Enable = 0`, REST and WebSocket endpoints accept requests without a
Bearer header. Non-local bind addresses are still rejected without enabled
Bearer authentication and a non-empty API key. Do not expose an unauthenticated
listener outside localhost.

## Testing

The REST contract is also available as [docs/openapi.yaml](docs/openapi.yaml).

Build and run the automatic EventBus unit tests:

```bash
cmake -S /path/to/azerothcore-wotlk -B /path/to/azerothcore-wotlk/build \
  -DBUILD_TESTING=ON
cmake --build /path/to/azerothcore-wotlk/build --target server_api_tests --parallel
ctest --test-dir /path/to/azerothcore-wotlk/build \
  -R server_api_tests --output-on-failure
```

Run API contract tests against a running worldserver:

```bash
SERVER_API_KEY='test-secret-key' ./tests/test_api_contract.sh
```

For labelled manual curl checks, run:

```bash
./tests/api-curl-checks.sh
```

Run the WebSocket protocol smoke test with Node.js 22+:

```bash
node ./tests/test-websocket.js
```

The test validates the handshake, `subscribe`, `ping/pong` and JSON event
envelopes when a telemetry event is produced during the timeout.

Run a read-only REST load smoke test:

```bash
./tests/load-api.sh
```

Defaults are 100 requests with concurrency 10. Override them with
`SERVER_API_REQUESTS`, `SERVER_API_CONCURRENCY` and
`SERVER_API_LOAD_ENDPOINT`.

Run a WebSocket load smoke test:

```bash
SERVER_API_WS_CLIENTS=10 \
SERVER_API_WS_DURATION_MS=5000 \
node ./tests/load-websocket.js
```

The client count must not exceed `ServerApi.WebSocket.MaxClients`.

After rebuilding with the client limit enabled, verify rejection above the
limit:

```bash
SERVER_API_WS_LIMIT_TEST_CLIENTS=51 \
node ./tests/test-websocket-limit.js
```

The test passes when at least one connection is rejected with the configured
`503 WS_CLIENT_LIMIT` behavior.

## Development

The module follows the official [AzerothCore skeleton-module](https://github.com/azerothcore/skeleton-module)
layout. Build the core with precompiled headers disabled while developing the module:

```bash
cmake -S /path/to/azerothcore-wotlk -B /path/to/azerothcore-wotlk/build -DNOPCH=1
cmake --build /path/to/azerothcore-wotlk/build --target worldserver
```

Read [docs/project-context.md](docs/project-context.md) and [docs/architecture.md](docs/architecture.md)
before implementing features.
