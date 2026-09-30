# Integration guide

## Reading runtime state

Use REST for snapshots and WebSocket for changes:

```text
REST snapshot -> initial state
WebSocket     -> realtime updates
```

Do not use the database as the source for online positions, active groups or
active maps. The module reads those values on the world thread and publishes
copyable snapshots to HTTP workers.

## Publishing an event from another module

Include the public EventBus header and publish only small, serializable values:

```cpp
#include "ServerApi/EventBus.h"

ServerApi::Publish("dungeon.clear", {
    {"mapId", std::to_string(mapId)},
    {"instanceId", std::to_string(instanceId)}
});
```

The event is delivered asynchronously to WebSocket subscribers. Do not pass
`Player*`, `Map*`, `Group*`, `WorldSession*` or other raw core pointers in the
event data. Keep event producers on the world thread and send strings only.

## Registering a module API

A feature module can register one handler for its own namespace. Registration is
performed during module startup and does not require a token: all modules are
compiled into the trusted worldserver process. The registry rejects invalid
names, duplicate registrations and conflicting namespaces.

```cpp
#include "ServerApi/ModuleRegistry.h"

ServerApi::GetModuleRegistry().Register(
    {"dungeon-clear", "1.0.0", {"dungeons", "runs"}},
    [](ServerApi::ModuleApiRequest const& request)
        -> std::optional<ServerApi::ModuleApiResponse>
    {
        if (request.method != "GET" || request.path != "/api/v1/mod/dungeon-clear/runs")
            return std::nullopt;

        return ServerApi::ModuleApiResponse{
            200, "OK", BuildRunsSnapshotJson(), {}};
    });
```

The handler is called on the API IO worker and receives the optional authenticated
identity in `request.identity`. It must not access `Player*`,
`Map*`, `WorldSession*` or other world objects. It may read a module-owned,
thread-safe value snapshot. Mutations must enqueue a world-thread command and
return `202 Accepted` only when the enqueue succeeds. Provider-based authentication, global
rate limiting and the `/api/v1/mod/<module>/...` namespace boundary are enforced by
`mod-server-api`.

Handlers should return `std::nullopt` for paths they do not own. Exceptions are
converted to a generic `500 MODULE_HANDLER_FAILED` response and are not allowed
to escape into the API worker.

## Registering an authentication provider

An integration module can register a provider factory in the public registry
before the API listener starts:

```cpp
#include "ServerApi/Authentication.h"

ServerApi::GetAuthenticationProviderRegistry().Register("my-provider", []
{
    return std::make_unique<MyAuthenticationProvider>();
});
```

Register during module initialization, not from an API request. The selected
provider is named by `ServerApi.Auth.Provider`; provider-owned settings belong
under `ServerApi.Auth.<provider-name>.*`. Providers run synchronously on the
single API I/O worker and must use a fast local check. They cannot access
world objects or perform blocking database/network calls. A provider used on a
non-local bind must return `true` from `RequiresAuthentication()`.

## Adding a new REST read endpoint

1. Collect data in `ServerSnapshot` on the world thread.
2. Publish immutable snapshot storage under the snapshot mutex and copy DTOs
   only after releasing it.
3. Serialize only the copied DTO in `ApiServer.cpp`.
4. Rely on the shared authentication provider, then apply path/query validation and bounded payload rules.
5. Update `README.md`, `docs/openapi.yaml` and `docs/project-context.md`.

## Adding a write operation

Never mutate AzerothCore objects from the HTTP worker. Enqueue a lambda in the
bounded `ServerApi::CommandQueue` and execute it from `ServerApiWorldScript`
on the next world update. Commands must remain lightweight and are drained with
a 2 ms budget per update. Return `202 Accepted` only after enqueue succeeds.

## Optional playerbots integration

`GET /api/v1/bots` is compiled behind `MOD_PLAYERBOTS`. Without the optional
module it returns `501 NOT_SUPPORTED`; the server API itself remains buildable.
Bot invite/kick operations require the same world-thread command queue and are
not enabled yet.
