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

Повторный `cmake -S . -B build` обязателен после добавления нового модуля,
`.cpp`-файла или `.conf.dist`: AzerothCore обнаруживает modules и формирует
список исходников и `ModulesLoader.cpp` на этапе конфигурации. `cmake --install build`
устанавливает module config в каталог конфигурации выбранного
`CMAKE_INSTALL_PREFIX`.

После конфигурации проверьте, что в выводе CMake модуль `mod-server-api` не отключён.
По умолчанию API уже включён на localhost и работает без аутентификации:

```ini
ServerApi.Enable = 1
ServerApi.BindAddress = "127.0.0.1"
ServerApi.Auth.Provider = "none"
ServerApi.Port = 7878
```

Для проверки HTTP после включения модуля:

```bash
curl -i http://127.0.0.1:7878/health
curl -i http://127.0.0.1:7878/ready
curl -i http://127.0.0.1:7878/api/v1/server
```

Ожидаемые тела ответов:

```json
{"status":"ok"}
{"status":"ready"}
```

Чтобы использовать аутентификацию, зарегистрируйте C++ provider в
скомпилированном модуле и задайте `ServerApi.Auth.Provider = "<name>"`.
Неизвестный provider останавливает listener; non-local bind требует provider,
который возвращает `RequiresAuthentication() == true`. Модуль не предоставляет
TLS: удалённый доступ организуйте через доверенный TLS proxy или защищённый
tunnel. При изменении provider, bind или порта требуется перезапуск worldserver.

Smoke-тест всех endpoints находится в `scripts/test-api.sh`:

```bash
./scripts/test-api.sh
```

Для проверки detail endpoint добавьте `SERVER_API_PLAYER_GUID` с GUID online
персонажа.

Расширенный контрактный тест проверяет публичные health/readiness endpoints,
REST с провайдером `none`, ответы для несуществующей интеграции и ошибки
фильтров:

```bash
./tests/test_api_contract.sh
```

Unit-тесты API-модуля, включая контракт провайдера аутентификации,
подключаются к `BUILD_TESTING=ON` и запускаются через CTest:

```bash
cmake -S /Users/sergeybolshanin/Documents/acore/azerothcore-wotlk \
  -B /Users/sergeybolshanin/Documents/acore/azerothcore-wotlk/build \
  -DBUILD_TESTING=ON
cmake --build /Users/sergeybolshanin/Documents/acore/azerothcore-wotlk/build \
  --target server_api_tests --parallel
ctest --test-dir /Users/sergeybolshanin/Documents/acore/azerothcore-wotlk/build \
  -R server_api_tests --output-on-failure
```
