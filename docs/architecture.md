# `mod-server-api` architecture

## Current implementation boundary

The current implementation contains:

- `ServerApiWorldScript` for configuration, startup, periodic snapshot refresh and shutdown;
- `ServerApi::ApiServer`, a small Boost.Asio HTTP listener;
- `ServerApi::EventBus`, a bounded asynchronous event queue;
- local health endpoints: `GET /health` and `GET /ready`;
- Bearer-protected runtime endpoints for server, players, groups, instances and optional integrations;
- administrative account endpoints with world-thread queued mutations;
- Bearer-protected WebSocket endpoint: `GET /ws/v1/events` with subscriptions and ping/pong.

Public headers live under `src/ServerApi/` because AzerothCore's automatic
module integration exports include directories recursively from the module
`src/` tree.

The listener is disabled by default and binds to `127.0.0.1` by default. When
enabled, Bearer authentication with a non-empty API key is required by default.
For trusted localhost deployments `ServerApi.Auth.Enable = 0` disables auth for
REST and WebSocket endpoints. Non-local binds still require Bearer auth. Health
endpoints remain unauthenticated for process probes; authenticated versioned
endpoints return `401` without a valid `Authorization: Bearer <key>` header.
Sampling intervals can be reloaded while the server is running. Listener,
authentication and request/WebSocket limit changes require a worldserver
restart because active IO sessions own an immutable configuration copy.

## Thread model

```text
world thread
    ├── load config
    ├── start/stop API lifecycle
    ├── drain bounded command queue on every update
    ├── refresh immutable server snapshot every second
    └── publish lightweight ApiEvent values

EventBus worker
    └── invoke subscribed handlers

API IO worker
    ├── accept HTTP connections and write responses
    └── read server snapshot; never access core game objects
```

The HTTP adapter handles one request per connection; WebSocket connections are
upgraded separately and stay on the API IO worker. Runtime values are collected
on the world thread and copied into a mutex-protected snapshot; the API worker
only serializes that copy. Account responses also take online IP/latency from the
snapshot instead of resolving a live `WorldSession` on the API thread. EventBus
callbacks are posted into the API IO context and each WebSocket has a bounded
write queue. It does not pass `Player*`, `Map*`,
`Group*`, databases, or client sockets across the thread boundary.

## Dependency decision

The AzerothCore checkout already uses Boost.Asio and the configured Boost
installation provides Beast. Beast is used for the WebSocket upgrade and frame
transport; the current REST responses still use small fixed/manual JSON
builders because no project-wide JSON DTO dependency is selected.

## Boundaries for the next phases

- Authentication, global rate limiting and versioned REST endpoints are implemented;
  secret rotation and authorization scopes remain future work.
- General REST DTO serialization still needs a selected JSON/HTTP adapter;
  shared escaping and request helpers cover the current manual builders.
- Game-state reads use a world-thread snapshot; no raw core pointers cross into API workers.
- State-changing requests must use a world-thread command queue.
- EventBus queue overflow must be observable and policy-driven before telemetry is connected.
- WebSocket sessions have bounded per-client queues; overflow closes the session.
