# Dynamic Island for Windows (custom)

## Поддержать проект
Dynamic Island бесплатный и со всеми функциями. Поддержать разработку можно донатом:
- **Донат EU** — ссылка скоро появится
- **Донат RU** — ссылка скоро появится

- `windhawk/` — рабочая версия-мод для Windhawk (1.4.0-custom.20). Не удаляется до завершения перехода.
- `docs/` — документация проекта: PROJECT_CONTEXT, CHANGELOG, TODO, STANDALONE_AUDIT (план перехода на самостоятельное приложение).
- `src/` — автономная версия (без Windhawk), сборка через CMake/MSVC, автоматически в GitHub Actions.
- `res/` — манифест и ресурсы `DynamicIsland.exe`.

## Сборка
GitHub Actions собирает при каждом изменении (вкладка Actions → последний запуск → Artifacts: `DynamicIsland-…`).
Локально: Visual Studio Build Tools (2022 или новее) + Windows SDK, затем
`cmake -S . -B build -A x64` и `cmake --build build --config Release`.
