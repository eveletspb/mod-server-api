# `mod-server-api` architecture

## Current implementation boundary

The current implementation contains:

- `ServerApiWorldScript` for configuration, startup, periodic snapshot refresh and shutdown;
- `ServerApi::ApiServer`, a small Boost.Asio HTTP listener;
- `ServerApi::EventBus`, a bounded asynchronous event queue;
- local health endpoints: `GET /health` and `GET /ready`;
- Runtime endpoints for server, online players, all current-realm characters, groups, instances and optional integrations, protected by the configured authentication provider;
- WebSocket endpoint `GET /ws/v1/events` with subscriptions and ping/pong, protected by the same provider.

Public headers live under `src/ServerApi/` because AzerothCore's automatic
module integration exports include directories recursively from the module
`src/` tree.

## Architectural ownership

`mod-server-api` is an infrastructure and transport module. Its ownership is
limited to HTTP/WebSocket access to worldserver runtime state, authentication,
limits, snapshots, the world-thread command queue, the EventBus, common
contracts, and routing for registered module capabilities.

It must not own domain state or domain rules for accounts, dungeon runs, raids,
rosters, gear, talents, premades, history, or battle logs. Those capabilities
belong to their feature modules. A feature module may expose a small
value-type integration bridge containing read snapshots, world-thread commands
and serializable events. The API module should not depend on the feature
module's internal managers or game-object pointers.

The intended dependency direction is:

```text
feature module -> ServerApi integration contract
mod-server-api -> registered capability adapter
```

The read-only character catalog is a core realm-wide administrative endpoint,
implemented in `CharactersApi` rather than the HTTP router. Character DB reads
use `CharacterDatabase.AsyncQuery`; the API IO worker polls its own callback
processor. This is a narrow core-data exception and does not change the schema
or expose account credentials, bags, bank or mail.

New domain-specific behavior should not be added directly to `ApiServer.cpp`.
The account API is temporarily removed. Dungeon routes remain a compatibility
adapter and should be moved behind this boundary incrementally.

Feature modules register a single handler through `ModuleRegistry`. The handler
owns only `/api/v1/mod/<module-name>/...`; registration rejects invalid names,
duplicate module names and duplicate capabilities. The API worker copies the
handler under the registry lock and invokes it after releasing the lock, so a
handler may not block registry operations or access world objects. Handler
exceptions become a generic `500 MODULE_HANDLER_FAILED` response.

The listener is enabled by default on `127.0.0.1`; `ServerApi.Auth.Provider`
defaults to `none`, which selects `NoAuthProvider`. Non-local binds require a
provider whose `RequiresAuthentication()` returns true. Health endpoints remain
public, while versioned REST and WebSocket requests pass through the configured
provider. The provider receives a read-only request context and runs
synchronously on the API I/O worker; it must not block or access world objects.
Identity is passed to module API handlers and retained by WebSocket sessions.
Authorization scopes and RBAC are not implemented. The listener does not
provide TLS; remote access requires a trusted TLS-terminating proxy or protected
tunnel.
Sampling intervals can be reloaded while the server is running. Listener,
authentication and request/WebSocket limit changes require a worldserver
restart because active IO sessions own an immutable configuration copy.

## Thread model

```text
world thread
    ├── load config
    ├── start/stop API lifecycle
    ├── drain bounded command queue with a 2 ms budget on every update
    ├── refresh immutable server snapshot every second
    └── publish lightweight ApiEvent values

EventBus worker
    └── invoke subscribed handlers

API IO worker
    ├── accept HTTP connections and write responses
    ├── poll the API-owned CharacterDatabase callback processor
    └── read immutable runtime snapshots; never access core game objects

Playerbots account-type cache worker (only when mod-playerbots is compiled)
    └── refresh account type values through its synchronous module DB pool
```

The HTTP adapter handles one request per connection; WebSocket connections are
upgraded separately and stay on the API IO worker. Runtime values are collected
on the world thread and copied into a mutex-protected snapshot; the API worker
only serializes that copy. Vector copies happen after releasing the snapshot
mutex, so HTTP serialization cannot hold the world thread on a large copy. Map
and instance data are collected in one map traversal per refresh. EventBus
uses separate queue and subscription mutexes, so world-thread publication does
not wait for subscription matching. Callbacks are posted into the API IO context
and each WebSocket has a bounded
write queue. It does not pass `Player*`, `Map*`,
`Group*`, databases, or client sockets across the thread boundary.

`CharactersApi` returns Character DB rows to the API IO worker through an
API-owned `QueryCallbackProcessor`, not the world query processor. Requests
are bounded to 32 concurrent operations; list scans use bounded database pages
and continue through an exclusive GUID cursor. Only copied DTO values cross
from DB callbacks into response builders. For online records, status and
location are overlaid from the immutable world-thread snapshot. The optional
Playerbots account-type cache is copied from its dedicated read-only worker;
the synchronous Playerbots DB pool is never called from world or HTTP threads.

## Dependency decision

The AzerothCore checkout already uses Boost.Asio and the configured Boost
installation provides Beast. Beast is used for the WebSocket upgrade and frame
transport; the current REST responses still use small fixed/manual JSON
builders because no project-wide JSON DTO dependency is selected.

## Boundaries for the next phases

- Provider-based authentication, global rate limiting and versioned REST endpoints are implemented;
  provider-specific credential rotation and authorization scopes remain the responsibility of providers/future work.
- General REST DTO serialization still needs a selected JSON/HTTP adapter;
  shared escaping and request helpers cover the current manual builders.
- Runtime game-state reads use a world-thread snapshot; no raw core pointers cross into API workers. Character DB data is read asynchronously by the API-owned callback processor.
- State-changing requests must use a world-thread command queue.
- Account routes are not exposed until their operations have a non-blocking execution model.
- EventBus queue overflow must be observable and policy-driven before telemetry is connected.
- WebSocket sessions have bounded per-client queues; overflow closes the session.
