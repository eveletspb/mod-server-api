# План развития архитектурной границы `mod-server-api`

## Цель

Сохранить `mod-server-api` чистым инфраструктурным модулем, который предоставляет
единый безопасный API для worldserver, его команд, событий и подключённых
модулей. Предметная логика остаётся у владельца соответствующей capability.

## Принципы

- API-модуль не владеет domain state и domain rules.
- Runtime game state читается через world-thread snapshots.
- Изменения выполняются через bounded world-thread command queue.
- Межмодульный контракт использует value types; raw core pointers запрещены.
- HTTP/WebSocket auth, limits, общий error contract и transport принадлежат API.
- Семантическая валидация payload и lifecycle принадлежат feature module.
- Account endpoints временно удалены; dungeon endpoints меняются постепенно.

## Этапы

### 1. Зафиксировать границу — текущий этап

- [x] Обновить `docs/project-context.md`.
- [x] Обновить `docs/architecture.md`.
- [x] Зафиксировать отдельный план разработки.
- [ ] Составить таблицу endpoint ownership: core или external capability.
- [x] Удалить account routes до появления неблокирующей модели исполнения.

### 2. Ввести module capability contract

- [x] Определить регистрацию module name, version и capabilities.
- [x] Добавить metadata endpoint `GET /api/v1/modules`.
- [x] Добавить unit tests для реестра и конфликтующей регистрации.
- [x] Определить интерфейс module API handler с namespace boundary.
- [x] Определить value-type request/response для module API.
- [x] Защитить dispatch от исключений module handler.
- [x] Добавить dispatch зарегистрированных module API handlers.
- [ ] Определить интерфейсы read snapshot, world-thread command и event.
- [ ] Разделить transport-level validation и domain-level validation.
- [ ] Определить typed operation response: `operationId`, `status`, `error`.
- [ ] Добавить contract tests для регистрации, неизвестной capability и
      переполнения command queue.

### 3. Убрать доменную логику из HTTP routing

- [x] Удалить account routes и `AccountService`, выполнявший тяжёлые операции
      в world thread.
- [x] Удалить прямые dungeon-clear маршруты из API-модуля; дальнейшая
      интеграция должна принадлежать feature-модулю.
- [x] Удалить изолированный legacy account implementation.
- [ ] Запретить добавление новых domain-specific обработчиков непосредственно
      в `ApiServer.cpp`.
- [ ] Обновить OpenAPI и `README.md` после каждого изменения контракта.

### 4. Расширить dungeon/raid интеграции вне API-модуля

- [ ] В `mod-dungeon-clear` экспортировать catalog, wing, gear, limits,
      roster, plans и typed run snapshots.
- [ ] Создать отдельный `mod-raid-runner` capability для candidate search,
      premade, lifecycle, history и battle log.
- [ ] Подключать оба модуля через value-type bridges и EventBus.
- [ ] Не переносить их managers, persistence и бизнес-правила в
      `mod-server-api`.

### 5. Проверка и миграция

- [ ] Добавить архитектурную проверку зависимостей и ownership routes.
- [ ] Проверить thread-affinity, logout, delayed commands и module unload/
      disable сценарии.
- [ ] Зафиксировать deprecation policy для legacy dungeon routes.
- [ ] Выполнить релевантные unit/contract tests и `git diff --check`.

## Что не входит в этот план

- Реализация account, dungeon или raid бизнес-логики в `mod-server-api`.
- Перенос domain persistence в API-модуль.
- Унификация всех предметных DTO в один гигантский общий контракт.
- Полный rewrite существующих endpoint’ов до появления реального consumer.
