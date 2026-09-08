# AzerothCore hooks used by `mod-server-api`

All hooks are implemented in `src/ServerApiModule.cpp`.

## `ServerApiWorldScript`

- `WORLDHOOK_ON_BEFORE_CONFIG_LOAD` reads and validates `ServerApi.*` settings.
- `WORLDHOOK_ON_STARTUP` starts EventBus and the API IO thread when enabled.
- `WORLDHOOK_ON_UPDATE` drains commands and refreshes snapshots and events on
  the world thread.
- `WORLDHOOK_ON_SHUTDOWN` stops the listener and joins module-owned workers.

## `ServerApiPlayerScript`

- `PLAYERHOOK_ON_LOGIN` publishes `player.login`.
- `PLAYERHOOK_ON_LOGOUT` publishes `player.logout` before player teardown.
- `PLAYERHOOK_ON_PLAYER_JUST_DIED` publishes `player.death`.

Player hooks copy event fields during the call and never pass `Player*` to an
API worker.

Group, map, combat and instance changes are derived from periodic world-thread
snapshots rather than separate hooks. Hooks must publish immutable values;
raw `Player*`, `Map*`, `Group*` and similar pointers must not be passed to API
workers.

The module loader is `Addmod_server_apiScripts()` in
`src/server_api_loader.cpp`; it delegates registration to
`AddServerApiScripts()`.
