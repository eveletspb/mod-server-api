# Events

Realtime events are delivered through the WebSocket endpoint:

```text
ws://127.0.0.1:7878/ws/v1/events
```

The handshake uses the provider selected by `ServerApi.Auth.Provider`.
The default `none` provider accepts local connections without credentials;
custom providers inspect the handshake request using the same authentication
contract as REST endpoints.

## Client messages

```json
{"type":"subscribe","events":["player.*","instance.*"]}
{"type":"unsubscribe"}
{"type":"ping"}
```

Supported patterns are exact event names and trailing wildcards such as
`player.*`. Each WebSocket has a bounded outgoing queue.

## Event envelope

```json
{
  "type": "player.login",
  "timestamp": 1788773379000,
  "version": 1,
  "data": {
    "guid": "123"
  }
}
```

Current events:

| Event | Data |
|---|---|
| `server.started` | Empty object |
| `server.stopping` | Empty object |
| `player.login` | `guid`, `name`, `level`, `mapId`, `zoneId` |
| `player.logout` | `guid`, `name`, `level`, `mapId`, `zoneId` |
| `player.death` | `guid`, `name`, `level`, `mapId`, `zoneId` |
| `player.position` | `guid`, `mapId`, `x`, `y`, `z`, `orientation` |
| `combat.snapshot` | `guid`, `mapId`, `victimGuid`, `health`, `maxHealth`, `power`, `maxPower` |
| `group.created` | `id`, `leaderGuid`, `raid`, `members` count |
| `group.updated` | `id`, `leaderGuid`, `raid`, `members` count |
| `group.disbanded` | Last known group data |
| `instance.started` | `instanceId`, `mapId`, `difficulty`, `players` |
| `instance.updated` | `instanceId`, `mapId`, `difficulty`, `players` |
| `instance.stopped` | Last known instance data |

`player.position` and `combat.snapshot` are snapshot-driven and sampled. They
do not represent every movement packet or damage event.

The recommended load-test baseline is 10 WebSocket clients. The listener
rejects new connections after `ServerApi.WebSocket.MaxClients` is reached.

Telemetry events have lower priority under EventBus backpressure and may be
dropped when the bounded queue is full. Lifecycle and control events keep the
normal/critical delivery path whenever capacity is available. The
`droppedEvents` metric includes telemetry rejected at capacity and telemetry
evicted to make room for a higher-priority event.
