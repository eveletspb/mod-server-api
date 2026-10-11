# Repository Guidelines

## Project Structure

`mod-server-api` is a C++ AzerothCore module that adds HTTP and WebSocket APIs
to `worldserver`. Core scripts and services live in `src/`; reusable API
interfaces are under `src/ServerApi/`, and event delivery is in `src/events/`.
The module loader is `src/server_api_loader.cpp`. Default settings are in
`conf/mod-server-api.conf.dist`; CMake integration is in
`mod-server-api.cmake`. Unit tests and API checks are in `tests/`; `scripts/`
and `apps/ci/` contain smoke and style checks. `mod-playerbots` is optional.

## Build and Development

Run core build commands from the AzerothCore source root, with this repository
at `modules/mod-server-api`:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --target worldserver --parallel
ctest --test-dir build -R server_api_tests --output-on-failure
```

The CTest target runs the module's GoogleTest unit tests. Run
`source modules/mod-server-api/apps/ci/ci-codestyle.sh` from the core root for
repository style checks. With a configured, running server,
the following checks the live REST contract:

```bash
SERVER_API_URL=http://127.0.0.1:7878 \
    modules/mod-server-api/tests/test_api_contract.sh
```

`modules/mod-server-api/scripts/test-api.sh` provides a basic endpoint smoke
test.

## Coding Style

Follow nearby AzerothCore C++ conventions. `.editorconfig` specifies UTF-8,
four spaces, no tabs, trimmed trailing whitespace, and an 80-character line
limit. Use PascalCase for types and the existing naming style for functions
and members. Keep handlers and integrations within the established API
boundaries; avoid blocking world-thread work. Update `README.md` and
`conf/mod-server-api.conf.dist` when endpoint or configuration behavior
changes.

## Tests

Name C++ unit files `Test*.cpp` and cover observable behavior, validation,
and failure cases. REST contract and load checks in `tests/` need a running
API; set `SERVER_API_URL` and optional test variables documented in each
script. Report build, unit, and live-server checks separately in contributions.

## Commits and Pull Requests

Recent commits use short Conventional Commit-style subjects, for example
`feat(auth): add pluggable authentication providers` or `fix(readme): ...`.
Keep subjects lowercase and focused. Use the PR template: summarize changes,
link related issues, and list tests performed plus reproducible test steps.
Include API/config contract changes and note any checks not run.

## Configuration and Safety

Keep the default listener on localhost. Before using a non-loopback bind,
verify the configured authentication provider and limits in
`conf/mod-server-api.conf.dist` and `README.md`; never commit credentials.
Keep database-backed work asynchronous and outside the world thread.
