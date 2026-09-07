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

## Adding a new REST read endpoint

1. Collect data in `ServerSnapshot` on the world thread.
2. Add a copy-returning getter guarded by the snapshot mutex.
3. Serialize only the copied DTO in `ApiServer.cpp`.
4. Apply Bearer validation, path/query validation and bounded payload rules.
5. Update `README.md`, `docs/openapi.yaml` and `docs/project-context.md`.

## Adding a write operation

Never mutate AzerothCore objects from the HTTP worker. Enqueue a lambda in the
bounded `ServerApi::CommandQueue` and execute it from `ServerApiWorldScript`
on the next world update. Return `202 Accepted` only after enqueue succeeds.

## Optional playerbots integration

`GET /api/v1/bots` is compiled behind `MOD_PLAYERBOTS`. Without the optional
module it returns `501 NOT_SUPPORTED`; the server API itself remains buildable.
Bot invite/kick operations require the same world-thread command queue and are
not enabled yet.
