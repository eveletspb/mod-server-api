# mod-server-api

`mod-server-api` adds an HTTP web server to AzerothCore's `worldserver` and
provides a shared foundation for API integrations from third-party modules.
An integration registers its own handlers under
`/api/v1/mod/<module-name>/...`; clients use the common HTTP server, routing,
authentication and request limits, while the integration module retains
ownership of its domain logic and data. `GET /api/v1/modules` lists the
registered module capabilities.

![mod-server-api banner](banner.png)

Built-in endpoints cover health and readiness, server state and metrics,
online players, the database-backed character catalog, groups, and active
dungeon/raid instances. The module also provides a world-thread runtime
snapshot, a bounded in-process EventBus, and `/ws/v1/events` with subscriptions
and ping/pong. Authentication is selected through a provider registry and
defaults to `NoAuthProvider` on localhost; see [docs/auth.md](docs/auth.md).

## Installation

Place or clone this repository as `modules/mod-server-api` in the
AzerothCore source tree, then reconfigure and rebuild the core:

```bash
cmake -S . -B build
cmake --build build --target worldserver --parallel
```

The core CMake configuration automatically installs
`conf/mod-server-api.conf.dist` with the other module configuration files.
Copy it to the active configuration directory if needed and edit the
`ServerApi.*` options before starting `worldserver`.

## Requirements

- AzerothCore WotLK source tree.
- `mod-playerbots` is optional. Without it, the core HTTP API still builds;
  bot-specific account types are reported as `unknown`.

## License

This project is licensed under the [GNU Affero General Public License v3.0 only](LICENSE).

## Supported API

Default address:

```text
http://127.0.0.1:7878
```

The listener is enabled by default on localhost with
`ServerApi.Auth.Provider = "none"`. The configured provider handles versioned
HTTP endpoints and WebSocket handshakes; `/health` and `/ready` stay public.

### Health

These endpoints are public and intended for liveness/readiness probes.

| Method | Endpoint | Response |
|---|---|---|
| `GET` | `/health` | `{"status":"ok"}` |
| `GET` | `/ready` | `{"status":"ready"}` |

### Server

| Method | Endpoint | Auth |
|---|---|---|
| `GET` | `/api/v1/server` | configured provider |
| `GET` | `/api/v1/server/metrics` | configured provider |

`/api/v1/server` returns `realmId`, `serverTime`, `uptimeSeconds`,
`playersOnline` and `botsOnline`.

`/api/v1/server/metrics` returns `activeMaps`, `activeInstances`,
`playersOnline`, `botsOnline`, `eventQueue`, `commandQueue` and
`droppedEvents`.

Example:

```bash
curl \
  http://127.0.0.1:7878/api/v1/server

curl \
  http://127.0.0.1:7878/api/v1/server/metrics
```

### Players

`/api/v1/players` remains the online runtime snapshot. It is separate from the
database-backed all-character catalog documented below.

| Method | Endpoint | Auth |
|---|---|---|
| `GET` | `/api/v1/players` | configured provider |
| `GET` | `/api/v1/players/{guid}` | configured provider |

Supported list query parameters:

| Parameter | Description |
|---|---|
| `name` | Case-sensitive substring filter |
| `mapId` | Exact map ID filter |
| `limit` | Maximum number of records, capped at 1000; default 100 |

Example:

```bash
curl \
  'http://127.0.0.1:7878/api/v1/players?limit=10&mapId=0'

curl \
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

### Characters

The character API is read-only and reads the current realm's Character DB, plus
World DB item templates for item-level calculations. It includes online and
offline characters. Deleted rows are hidden by default;
`includeDeleted=true` includes soft-deleted rows in list results. Character
details always hide deleted rows and return `404` for a deleted or missing GUID.

| Method | Endpoint | Auth |
|---|---|---|
| `GET` | `/api/v1/characters` | configured provider |
| `GET` | `/api/v1/characters/{guid}` | configured provider |
| `GET` | `/api/v1/characters/item-levels?guids=...` | configured provider |
| `GET` | `/api/v1/characters/statistics` | configured provider |
| `GET` | `/api/v1/instance-binds` | configured provider |
| `POST` | `/api/v1/instance-binds/reset?mapId=...&difficulty=...&guids=...` | configured provider |

List parameters:

| Parameter | Description |
|---|---|
| `limit` | Page size, default 100, maximum 1000 |
| `cursor` | Exclusive GUID cursor from the prior response's `nextCursor` |
| `name` | Case-insensitive name prefix |
| `accountId`, `guildId`, `race`, `class`, `mapId`, `zoneId` | Exact match |
| `online`, `bot`, `includeDeleted` | Boolean (`true`/`false` or `1`/`0`) |
| `botType` | `player`, `random`, `addclass` or `unknown` |
| `minLevel`, `maxLevel` | Inclusive level bounds |
| `guids` | Comma-separated exact GUID list, at most 100 entries |
| `names` | Comma-separated exact character names, at most 40 entries |
| `includeTalents` | Include active talent spell IDs and server-resolved specialization summary |

Filters are combined with `AND`. Unknown or repeated parameters, invalid values,
and `minLevel > maxLevel` return `400 INVALID_FILTER`. The cursor is an
exclusive GUID: keep the same filters and pass the previous `nextCursor` to
continue. A response has this shape:

```json
{
  "data": [{"guid": 123, "name": "Papas", "online": true}],
  "hasMore": true,
  "nextCursor": 123
}
```

Results are ordered by ascending GUID and return `data`, `hasMore` and
`nextCursor`; the API does not calculate a total count. Dynamic filters use the
current immutable online snapshot. If a selective filter needs to scan more
than 10,000 database candidates, a page can contain fewer than `limit` rows;
continue with `nextCursor` while `hasMore` is true. Concurrent catalog/profile
requests are capped at 32 and excess requests receive `503`.

Each list entry includes `guid`, `accountId`, `name`, `race`, `class`, `gender`,
`level`, `guildId`, `guildName`, `online`, `available`, `bot`, `botType`, `deleted`,
`mapId`, `zoneId` and `location` (`mapId`, `zoneId`, `x`, `y`, `z` and
`orientation`). `available` is true when the character is offline or not in a
group. With `includeTalents=true`, entries also contain `talentSpells`, the
active specialization's learned spell IDs, plus `spec`, `talentPoints` (points
spent in each talent tree) and `suggestedRoles`, calculated from server DBC
data. A profile always includes these talent fields and adds `xp`, `money`, `talents` (`activeGroup` and
`totalGroups`), `equipment` entries (`slot` and `itemEntry`) and `professions`
(`skillId`, `name`, `value` and `max`).

The item-level endpoint accepts either up to 100 `guids` or up to 40 exact
`names`. It returns the rounded average `ItemLevel` for equipped inventory slots,
excluding shirt and tabard. It reads inventory from Character DB and item
templates from World DB inside the module.

The statistics endpoint returns average latency (`null` when no characters are
online) and online Alliance/Horde character counts.

### Instance binds

`GET /api/v1/instance-binds` returns the current realm's personal dungeon and
raid binds for online and offline characters. It reads `character_instance`,
`instance` and character identity/status fields; `inInstance` is derived from
the live character snapshot. Each row includes `guid`, `name`, `class`, `level`,
`online`, `mapId`, `mapName`, `isRaid`, `difficulty`, `permanent`, `extended`,
`resetAt` (Unix seconds), `completedEncounters` and `inInstance`.

`POST /api/v1/instance-binds/reset` accepts `mapId`, `difficulty` and a
comma-separated `guids` query parameter (1–100 unique nonzero GUIDs). The
operation is queued on the world thread and unbinds only the requested
map/difficulty through `InstanceSaveMgr::PlayerUnbindInstance`; it does not
change the global instance reset schedule or other character binds. The API
returns one result per GUID with `status` (`reset`, `skipped` or `failed`) and a reason
when skipped. A character currently in combat, inside that map, or no longer
bound is skipped. In cluster mode, a worldserver that does not own the map also
returns a skipped result. Database/request queue errors are returned as API errors, not
as successful per-character results. In cluster mode, GET reports database
binds while reset only runs on a worldserver assigned the requested map.

```json
{
  "results": [
    {"guid": 123, "status": "reset"},
    {"guid": 456, "status": "skipped", "reason": "Character is in combat."}
  ]
}
```

Online status, map, zone and coordinates are taken from the current runtime
snapshot for online characters. Other character fields are read from DB and
may reflect the last saved state. Bot sessions are detected from the runtime
snapshot; `random` and `addclass` types come from Playerbots account assignments.
Without Playerbots or its account-type table, `botType` is `unknown`. A
Playerbots bot on an ordinary player account can therefore have `bot=true` and
`botType=player` while online; offline bot status is inferred only from
Playerbots account type.

The profile includes core identity/level fields, account ID, guild, effective
location, equipped items in equipment slots, active talent group, and profession
skill values. Bags, bank, mail and account credentials are not exposed.

Examples:

```bash
curl \
  'http://127.0.0.1:7878/api/v1/characters?limit=100&name=pap&online=false&minLevel=20'

curl \
  'http://127.0.0.1:7878/api/v1/characters?limit=100&name=pap&cursor=123'

curl \
  'http://127.0.0.1:7878/api/v1/characters/123'
```

Character endpoint errors use `400 INVALID_FILTER` or `INVALID_GUID` for bad
input, `404 CHARACTER_NOT_FOUND` for a missing or deleted profile, and
`503 DATABASE_UNAVAILABLE` or `CHARACTER_QUEUE_FULL` when the database or
bounded request capacity is unavailable.

### Modules

`GET /api/v1/modules` returns the API capabilities registered by the running
worldserver modules. The endpoint is intentionally metadata-only: feature
modules own their domain state and rules, while `mod-server-api` provides the
transport, authentication, snapshots, commands and events used to expose them.

Each item contains `name`, `version` and a list of capability names.

### Groups

| Method | Endpoint | Auth |
|---|---|---|
| `GET` | `/api/v1/groups` | configured provider |
| `GET` | `/api/v1/groups/{id}` | configured provider |

Groups are collected from online players on the world thread. A group with no
online player is not visible in this runtime snapshot. Each item contains
`id`, `leaderGuid`, `raid` and the `members` array of character GUIDs.

### Instances

| Method | Endpoint | Auth |
|---|---|---|
| `GET` | `/api/v1/instances` | configured provider |
| `GET` | `/api/v1/instances/{instanceId}` | configured provider |

The response contains active dungeon and raid maps only. Each item contains
`instanceId`, `mapId`, `difficulty` and the current non-GM `players` count.
Persistent instance-save state and a `startedAt` timestamp are not exposed.

### Bots

| Method | Endpoint | Auth |
|---|---|---|
| `GET` | `/api/v1/bots` | configured provider |

When `mod-playerbots` is compiled into the worldserver, the endpoint returns
online bot players with the same basic fields as the player list. Without
`mod-playerbots`, it returns `501 NOT_SUPPORTED`; the server API module still
builds normally. Bot-specific invite/kick commands remain deferred until the
playerbots integration is connected to the command queue.

### Commands

`POST /api/v1/players/{guid}/kick` queues a player kick for execution on the
world thread. It does not run game-object operations from the HTTP worker.
The normal response is `202 Accepted`:

```bash
curl -i -X POST \
  http://127.0.0.1:7878/api/v1/players/123/kick
```

The command queue is bounded and receives at most 2 ms of execution time per
world update. If it is full, the API returns `503 COMMAND_QUEUE_FULL`. This
command is intentionally not included in the automatic contract test because
it changes server state and can disconnect a real player.

`POST /api/v1/players/{guid}/teleport` uses the same queue and requires
`mapId`, `x`, `y`, `z` and `orientation` query parameters:

```bash
curl -i -X POST \
  'http://127.0.0.1:7878/api/v1/players/123/teleport?mapId=0&x=-8949.95&y=-132.49&z=83.53&orientation=0'
```

### WebSocket events

Endpoint:

```text
ws://127.0.0.1:7878/ws/v1/events
```

The WebSocket handshake uses the configured authentication provider. After
connecting, send a compact JSON subscription message:

```json
{"type":"subscribe","events":["server.*","player.*","group.*","instance.*"]}
```

Supported client messages:

```json
{"type":"subscribe","events":["player.*"]}
{"type":"unsubscribe"}
{"type":"ping"}
{"type":"publish","event":"provider.metric","data":{"name":"cpu","value":"42"}}
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
`ServerApi.WebSocket.MaxClients` (default 10). The outgoing queue is capped by
both message count (default 256) and total bytes (default 2 MiB), including
messages waiting for the API I/O worker, so brief client read stalls have more
buffering headroom without allowing an unbounded backlog.
Incoming WebSocket messages are limited per connection; clients exceeding
`ServerApi.WebSocket.MaxMessagesPerSecond` (default 10,000) are disconnected.
Authenticated clients can publish events with the `publish` message. Four
worker threads parse publications and hand them to the EventBus in accepted
order. Delivery is fire-and-forget; when the bounded queue is full, new
publications are dropped. `ServerApi.WebSocket.MaxPublishQueue` controls the
publication and EventBus queue capacity (default 10,000). Publication queue,
drop and invalid-message counts are included in `/api/v1/server/metrics`.

### Server-Sent Events

The same EventBus is also available as a one-way SSE stream. Its event patterns
and JSON envelope match WebSocket. Pass comma-separated patterns in `events`:

```text
GET /sse/v1/events?events=server.*,player.*
```

Each SSE `data:` field contains the same JSON event object shown above. The
server sends a comment heartbeat every 15 seconds. Reconnecting clients receive
events from the current server process. Events named `rde.run.log.*` include a
process-specific `streamEpoch` and a server-wide `streamSequence` in the
envelope; SSE also sets `id: <streamEpoch>:<streamSequence>`. Reconnecting
clients can send that cursor in `Last-Event-ID`. The EventBus retains up to
8,192 RDE events or 16 MiB, whichever limit is reached first, including events
dropped from its dispatch queue. Replay sends retained events in cursor order,
streaming them through the bounded per-client queue; live events wait behind the
replay in a separately bounded buffer. A cursor older than retention resumes at
the oldest available event, exposing a sequence gap. A server restart changes
the epoch, so events from the previous process are not replayed. Other event
types and older servers omit the cursor fields.

Slow clients have a bounded per-connection queue and are disconnected when it
fills. SSE shares the configured `ServerApi.IOThreads` pool with HTTP and
WebSocket I/O; each connection stays serialized while independent connections
run concurrently. SSE writes combine up to 64 queued events or 64 KiB into one
socket write.
Character database API callbacks use a separate strand to keep their shared
request state serialized. SSE limits are independent from WebSocket:

```ini
ServerApi.SSE.Enable = 1
ServerApi.SSE.MaxSubscriptions = 20
ServerApi.SSE.MaxQueue = 256
ServerApi.SSE.MaxQueueBytes = 2097152
ServerApi.SSE.MaxClients = 10
ServerApi.SSE.HeartbeatSeconds = 15
```

The endpoint uses the configured API authentication provider. Inspect a local
stream with `curl -N`:

```bash
curl -N 'http://127.0.0.1:7878/sse/v1/events?events=server.*,player.*'
```

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
| `401` | Configured authentication provider rejected the request |
| `404` | Unknown endpoint or character/player not found |
| `405` | Known endpoint called with an unsupported method |
| `413` | Request exceeds `ServerApi.MaxRequestBytes` |
| `429` | Global HTTP request rate limit exceeded |
| `500` | Authentication provider failed or returned an invalid result |
| `501` | Optional integration is unavailable |
| `503` | Character database/request capacity, command queue or WebSocket client limit reached |

### cURL examples

Проверка доступности API без авторизации:

```bash
curl -i http://127.0.0.1:7878/health
curl -i http://127.0.0.1:7878/ready
```

Проверка API с провайдером `none` по умолчанию:

```bash
curl -i http://127.0.0.1:7878/api/v1/server
curl -i http://127.0.0.1:7878/api/v1/players
```

Примеры запросов:

```bash
curl -i \
  http://127.0.0.1:7878/api/v1/server

curl -i \
  http://127.0.0.1:7878/api/v1/server/metrics

curl -i \
  'http://127.0.0.1:7878/api/v1/players?limit=10'

curl -i \
  'http://127.0.0.1:7878/api/v1/players?name=Papas&mapId=0&limit=25'

curl -i \
  http://127.0.0.1:7878/api/v1/players/123

curl -i \
  http://127.0.0.1:7878/api/v1/groups

curl -i \
  http://127.0.0.1:7878/api/v1/groups/123

curl -i \
  http://127.0.0.1:7878/api/v1/instances

curl -i \
  http://127.0.0.1:7878/api/v1/instances/1

curl -i \
  http://127.0.0.1:7878/api/v1/bots
```

Проверка ошибок фильтров и неизвестного игрока:

```bash
curl -i \
  'http://127.0.0.1:7878/api/v1/players?mapId=invalid'

curl -i \
  'http://127.0.0.1:7878/api/v1/players?limit=0'

curl -i \
  http://127.0.0.1:7878/api/v1/players/999999999

curl -i \
  http://127.0.0.1:7878/api/v1/groups/not-a-number

curl -i \
  http://127.0.0.1:7878/api/v1/instances/999999999
```

## Configuration

```ini
ServerApi.Enable = 1
ServerApi.BindAddress = "127.0.0.1"
ServerApi.Port = 7878
ServerApi.MaxRequestBytes = 1048576
ServerApi.MaxRequestsPerSecond = 1000
ServerApi.IOThreads = 4
ServerApi.PositionUpdates.IntervalMs = 1000
ServerApi.CombatSnapshot.IntervalMs = 2000
ServerApi.WebSocket.Enable = 1
ServerApi.WebSocket.MaxFrameBytes = 16384
ServerApi.WebSocket.MaxSubscriptions = 20
ServerApi.WebSocket.MaxQueue = 256
ServerApi.WebSocket.MaxQueueBytes = 2097152
ServerApi.WebSocket.MaxClients = 10
ServerApi.WebSocket.MaxMessagesPerSecond = 10000
ServerApi.WebSocket.MaxPublishQueue = 10000
ServerApi.SSE.Enable = 1
ServerApi.SSE.MaxSubscriptions = 20
ServerApi.SSE.MaxQueue = 256
ServerApi.SSE.MaxQueueBytes = 2097152
ServerApi.SSE.MaxClients = 10
ServerApi.SSE.HeartbeatSeconds = 15
ServerApi.Auth.Provider = "none"
```

To use a custom provider:

```ini
ServerApi.Auth.Provider = "my-provider"
```

The provider must be registered by a compiled AzerothCore module before the API
starts. Unknown provider names stop the listener. The listener can bind to a
LAN or other non-loopback address with any provider. With `none`, all versioned
API endpoints accept anonymous requests from any host that can reach the
listener, so restrict access with your network security layer. The module does
not provide TLS; use a trusted TLS-terminating proxy or protected tunnel when
traffic crosses an untrusted network. Provider integration details are in
[docs/auth.md](docs/auth.md). The former
`ServerApi.Auth.Enable` and `ServerApi.Auth.ApiKey` settings are no longer read.

On startup, the module logs the selected provider name, listener, limits and
sampling intervals. Provider secrets are not included in the startup summary.

## Testing

The REST contract is also available as [docs/openapi.yaml](docs/openapi.yaml).

Build and run the automatic module unit tests:

```bash
cmake -S /path/to/azerothcore-wotlk -B /path/to/azerothcore-wotlk/build \
  -DBUILD_TESTING=ON
cmake --build /path/to/azerothcore-wotlk/build --target server_api_tests --parallel
ctest --test-dir /path/to/azerothcore-wotlk/build \
  -R server_api_tests --output-on-failure
```

Run API contract tests against a running worldserver:

```bash
./tests/test_api_contract.sh
```

For labelled manual curl checks, run:

```bash
./tests/api-curl-checks.sh
```

Run the WebSocket protocol smoke test with Node.js 22+:

```bash
node ./tests/test-websocket.js
```

The test validates the handshake, `subscribe`, `ping/pong`, publication and the
JSON event envelope.

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

Measure WebSocket publication throughput and end-to-end latency:

```bash
SERVER_API_WS_PUBLISH_RATE=10000 \
SERVER_API_WS_DURATION_MS=5000 \
node ./tests/load-websocket-publish.js
```

The output includes sent/received counts, measured send rate, latency percentiles
and publication queue metrics when the metrics endpoint is accessible.

After rebuilding with the client limit enabled, verify rejection above the
limit:

```bash
SERVER_API_WS_LIMIT_TEST_CLIENTS=51 \
node ./tests/test-websocket-limit.js
```

The test passes when at least one connection is rejected with the configured
`503 WS_CLIENT_LIMIT` behavior.

## AzerothCore integration

- Configuration: `conf/mod-server-api.conf.dist`, installed automatically by
  the core CMake configuration.
- SQL patches: none. The character catalog reads existing Character DB tables;
  it does not modify the schema.
- CMake hooks: none. `mod-server-api.cmake` adds the unit-test target when
  `BUILD_TESTING` is enabled.
- Core hooks: `WorldScript::OnBeforeConfigLoad`, `OnStartup`, `OnUpdate` and
  `OnShutdown`; `PlayerScript::OnPlayerLogin`, `OnPlayerLogout` and
  `OnPlayerJustDied`.

## Development

The module follows the official [AzerothCore skeleton-module](https://github.com/azerothcore/skeleton-module)
layout. Build the core with precompiled headers disabled while developing the module:

```bash
cmake -S /path/to/azerothcore-wotlk -B /path/to/azerothcore-wotlk/build -DNOPCH=1
cmake --build /path/to/azerothcore-wotlk/build --target worldserver
```

Read the [integration guide](docs/integration.md) and
[architecture notes](docs/architecture.md) before implementing features.
