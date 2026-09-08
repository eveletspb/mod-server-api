# Сборка `mod-server-api`

Сборку запускайте из корня AzerothCore, не из каталога модуля. Модуль
подключается корневым `modules/CMakeLists.txt`; отдельный `CMakeLists.txt`
внутри модуля не требуется.

```bash
cd /Users/sergeybolshanin/Documents/acore/azerothcore-wotlk
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSCRIPTS=static \
  -DMODULES=static \
  -DNOPCH=1 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --target worldserver --parallel
cmake --install build
```

Повторный `cmake -S . -B build` обязателен после добавления нового модуля или
новых `.conf.dist`: AzerothCore обнаруживает modules и формирует
`ModulesLoader.cpp` на этапе конфигурации. `cmake --install build`
устанавливает module config в каталог конфигурации выбранного
`CMAKE_INSTALL_PREFIX`.

После конфигурации проверьте, что в выводе CMake модуль `mod-server-api` не отключён. Для первого запуска API оставьте:

```ini
ServerApi.Enable = 0
ServerApi.BindAddress = "127.0.0.1"
ServerApi.Port = 7878
```

Для проверки HTTP после включения модуля:

```bash
curl -i http://127.0.0.1:7878/health
curl -i http://127.0.0.1:7878/ready
curl -i http://127.0.0.1:7878/api/v1/server
curl -i -H 'Authorization: Bearer change-me' http://127.0.0.1:7878/api/v1/server
```

Ожидаемые тела ответов:

```json
{"status":"ok"}
{"status":"ready"}
```

Перед включением API по умолчанию задайте непустой `ServerApi.Auth.ApiKey` и
используйте его в заголовке `Authorization: Bearer <key>`. Для доверенного
локального deployment можно установить `ServerApi.Auth.Enable = 0`; тогда
versioned API и WebSocket работают без токена. Для non-local bind auth остаётся
обязательным. Не коммитьте production key в репозиторий. При изменении bind
или порта требуется перезапуск worldserver.

Smoke-тест всех endpoints находится в `scripts/test-api.sh`:

```bash
SERVER_API_KEY='ваш-секретный-ключ' ./scripts/test-api.sh
```

Для проверки detail endpoint добавьте `SERVER_API_PLAYER_GUID` с GUID online
персонажа.

Расширенный контрактный тест проверяет тела ответов, auth failures и ошибки
фильтров:

```bash
SERVER_API_KEY='ваш-секретный-ключ' ./tests/test_api_contract.sh
```

Unit-тесты EventBus подключаются к `BUILD_TESTING=ON` и запускаются через CTest:

```bash
cmake -S /Users/sergeybolshanin/Documents/acore/azerothcore-wotlk \
  -B /Users/sergeybolshanin/Documents/acore/azerothcore-wotlk/build \
  -DBUILD_TESTING=ON
cmake --build /Users/sergeybolshanin/Documents/acore/azerothcore-wotlk/build \
  --target server_api_tests --parallel
ctest --test-dir /Users/sergeybolshanin/Documents/acore/azerothcore-wotlk/build \
  -R server_api_tests --output-on-failure
```
