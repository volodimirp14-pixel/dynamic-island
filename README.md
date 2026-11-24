# Dynamic Island for Windows (custom)

## Версии: пробная и Pro
| Функция | Пробная | Pro |
|---|:---:|:---:|
| Часы, дата и календарь | ✓ | ✓ |
| Музыкальный плеер с обложкой, спектром и перемоткой | ✓ | ✓ |
| Громкость, яркость, батарея | ✓ | ✓ |
| Темы, размер, положение, анимации, «Не беспокоить» | ✓ | ✓ |
| Сообщения Telegram и Discord в острове | — | ✓ |
| Bluetooth: подключение устройств и заряд наушников / геймпадов | — | ✓ |
| Умный игровой режим, игровой оверлей FPS/CPU, монитор железа | — | ✓ |
| Погода, акции, таймер фокусировки, кольцо прогресса | — | ✓ |
| Индикаторы микрофона и камеры | — | ✓ |
| Liquid Glass и акрил, цвет акцента, свои цвета и шрифт | — | ✓ |

**Купить Pro** — скоро (покупка пока не работает). Страница Pro: [volodimirp14-pixel.github.io/dynamic-island](https://volodimirp14-pixel.github.io/dynamic-island/).


- `windhawk/` — рабочая версия-мод для Windhawk (1.4.0-custom.20). Не удаляется до завершения перехода.
- `docs/` — документация проекта: PROJECT_CONTEXT, CHANGELOG, TODO, STANDALONE_AUDIT (план перехода на самостоятельное приложение).
- `src/` — автономная версия (без Windhawk), сборка через CMake/MSVC, автоматически в GitHub Actions.
- `res/` — манифест и ресурсы `DynamicIsland.exe`.

## Сборка
GitHub Actions собирает при каждом изменении обе версии (вкладка Actions → последний запуск → Artifacts: `DynamicIsland-Pro-…` и `DynamicIsland-Trial-…`).
Локально: Visual Studio Build Tools (2022 или новее) + Windows SDK, затем
`cmake -S . -B build -A x64` (Pro) или `cmake -S . -B build -A x64 -DDI_PRO=OFF` (пробная), затем `cmake --build build --config Release`.
