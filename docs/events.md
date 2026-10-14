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
{"type":"publish","event":"provider.metric","data":{"name":"cpu","value":"42"}}
```

Supported patterns are exact event names and trailing wildcards such as
`player.*`. Each WebSocket has a bounded outgoing queue.
Authenticated WebSocket clients may publish events. The event name is limited
to 128 bytes and `data` must be an object containing string values. Published
events are delivered to matching subscribers using the normal event envelope.
The server parses publications on four worker threads and publishes them in the
order they were accepted from WebSocket reads. Delivery is fire-and-forget; the
server does not acknowledge receipt or replay missed events.

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

Events named `rde.run.log.*` include a process-specific `streamEpoch` and a
server-wide `streamSequence` number in the envelope. The sequence is assigned
before the EventBus queue, so it also identifies events dropped by queue
backpressure. SSE sends `id: <streamEpoch>:<streamSequence>` and replays after a
matching `Last-Event-ID` cursor. The EventBus keeps at most 8,192 RDE events or
16 MiB, whichever limit is reached first. Replay is sent incrementally through
the bounded per-client SSE queue; live events arriving during replay wait behind
retained events in a separately bounded buffer. A cursor older than the retained
window resumes from the oldest available event and exposes a sequence gap. A
server restart changes the epoch and cannot replay the previous process. Other
event types and older servers omit these fields.

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
rejects new connections after `ServerApi.WebSocket.MaxClients` is reached
(default 10). Each client's outgoing backlog is bounded by both message count
and total bytes, including messages waiting for the API I/O worker. A slow
client is disconnected when either limit is reached.
Incoming WebSocket messages are limited per connection by
`ServerApi.WebSocket.MaxMessagesPerSecond` (default 10,000); clients exceeding
the limit are disconnected. The publication queue and EventBus are bounded by
`ServerApi.WebSocket.MaxPublishQueue` (default 10,000). New publications are
dropped when the queue is full. The `/api/v1/server/metrics` response exposes
`webSocketPublishQueue`, `webSocketPublishDropped` and
`webSocketPublishInvalid` for monitoring.

Telemetry events have lower priority under EventBus backpressure and may be
dropped when the bounded queue is full. Lifecycle and control events keep the
normal/critical delivery path whenever capacity is available. The
`droppedEvents` metric includes telemetry rejected at capacity and telemetry
evicted to make room for a higher-priority event.
