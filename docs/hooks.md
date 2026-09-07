# AzerothCore hooks used by `mod-server-api`

| Hook | Class | File | Purpose | Thread/lifetime notes |
|---|---|---|---|---|
| `WORLDHOOK_ON_BEFORE_CONFIG_LOAD` | `ServerApiWorldScript` | `src/ServerApiModule.cpp` | Reads and validates `ServerApi.*` settings before startup | Runs on the world lifecycle; only copies scalar/string config |
| `WORLDHOOK_ON_STARTUP` | `ServerApiWorldScript` | `src/ServerApiModule.cpp` | Starts EventBus and API IO thread when enabled | Does not access gameplay objects |
| `WORLDHOOK_ON_SHUTDOWN` | `ServerApiWorldScript` | `src/ServerApiModule.cpp` | Stops listener and joins API/EventBus workers | Must complete before module-owned worker state is destroyed |

No player, group, map, combat, death or instance hooks are registered yet. Future hooks must enqueue immutable values or build snapshots on the world thread; raw `Player*`, `Map*`, `Group*` and similar pointers must not be passed to API workers.

The module loader is `Addmod_server_apiScripts()` in `src/server_api_loader.cpp`; it delegates registration to `AddServerApiScripts()`.
