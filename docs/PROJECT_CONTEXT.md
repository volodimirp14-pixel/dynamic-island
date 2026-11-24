# PROJECT_CONTEXT — Dynamic Island for Windows (кастомная версия)

> Главный документ проекта. Обновляется после каждого существенного изменения.
>
> **Новое направление (2026-11-24):** переход на самостоятельное Windows-приложение без Windhawk (остров + отдельный Launcher). Аудит и план — `STANDALONE_AUDIT.md`. Пока план не согласован и автономная версия не проверена, рабочей остаётся Windhawk-версия, описанная ниже.
> Последнее обновление: 2026-11-24 — версия `1.4.0-custom.20` (перелистывание вкладок без изменения размера текста: только сдвиг на целые пиксели + прозрачность). До неё: `1.4.0-custom.19` (в список акций добавлены SpaceX и S&P 500). Вкладка «Акции» — с `1.4.0-custom.18`. До неё: `1.4.0-custom.17` (материал Liquid Glass). Предыдущая — `1.4.0-custom.16` (полная очистка: удалены тексты песен, буфер обмена, файловый лоток, Quick Lookup, Caps Lock, анимации переходов плеера, полоса обратного отсчёта, пилюля «Не беспокоить» Windows и все уведомления Windows, кроме Telegram и Discord).

## 0. Автономная версия (новая основа проекта)
Репозиторий: https://github.com/volodimirp14-pixel/dynamic-island (сборка — GitHub Actions, Windows + MSVC).
Планы и решения — `STANDALONE_AUDIT.md`. Все новые изменения делаются **здесь**, а не в Windhawk-моде.

| Часть | Где | Что |
|---|---|---|
| Точка входа | `src/island/main.cpp` | аргументы (`--quit/--reload/--show/--progress N/--diagnose`), один экземпляр, проверка Windhawk-копии, цикл сообщений |
| Остров | `src/island/IslandCore.cpp` + `Island.h` | весь код мода (рендер, анимации, источники данных, Telegram/Discord, игры); API `di::island::Start/Stop/ApplySettings/Show/SetProgress` |
| Слежение за настройками | `src/island/settings/` | перечитывает `settings.json` после сохранения |
| Команды | `src/island/app/Control.*`, `src/common/Ipc.h` | скрытое окно `DynamicIsland.Control`, `WM_COPYDATA` |
| Лог | `src/common/Log.*` | `%LOCALAPPDATA%\DynamicIsland\logs\island.log`, 5 МБ × 3 |
| Проверка | `src/island/diag/` | окно `--diagnose` |
| Ресурсы | `res/` | манифест (PerMonitorV2, UTF-8, asInvoker), версия, схема |
| Общая библиотека | `src/common/` | пути, лог, настройки (`Settings.*`, схема `settings_schema.json`), клиент команд `IpcClient`, версия |
| Launcher | `src/launcher/` | `main.cpp` (окно + WebView2 + обработка сообщений страницы), `IslandProcess` (запуск/остановка острова), `Autostart` (ключ Run), `ui/index.html` (весь интерфейс) |
| Скрипты | `tools/` | `yaml_to_schema.py`, `port_core.py`, `translate_schema.py` |

**Launcher ↔ страница:** `chrome.webview.postMessage` (JSON). Страница → C++: `init`, `status`, `set {key,value}`, `reset`, `start`, `stop`, `restart`, `kill`, `autostart {enabled}`, `open {what}`. C++ → страница: `init {schema, values, version…}`, `status {running, autostart}`, `saved`, `reset`, `action`.
**Launcher ↔ остров:** запись `settings.json` + команда `reload` (`WM_COPYDATA`), `quit` для остановки; запуск — `CreateProcess` соседнего `DynamicIsland.exe`.

Файлы пользователя: `%APPDATA%\DynamicIsland\settings.json`, `state.json`; логи — `%LOCALAPPDATA%\DynamicIsland\logs\`.
Ключи настроек те же, что были в Windhawk (`Telegram.Enabled`, `Themes.BackdropMaterial` …). Новая настройка = запись в `src/common/settings_schema.json` (ключ, тип, значение по умолчанию, название и подсказка по-русски) + чтение `di::settings::GetInt/GetString` в `LoadSettings()`. Launcher покажет её сам; раздел и подраздел задаются в `ui/index.html` (`PAGES`, `SUBSECTIONS`), условия активности — `ACTIVE_IF`.

### Bluetooth-значок (автономная версия, 0.7–0.8)
- Состояние: `BluetoothRegistry` / `Bt()` (никогда не освобождается): известные устройства, подключённые сейчас, срок следующего обновления значка.
- Поток `BluetoothThreadProc`: два `DeviceWatcher` (classic, BLE; с `System.Devices.Aep.IsConnected`), раз в секунду — проверка подключения (каждые 10 с или перед обновлением значка) и `RefreshBluetoothBadge` (раз в 60 с; 5 с, пока подключены наушники; события сдвигают раньше).
- Наушники/колонки показываются, только пока у Windows есть активное звуковое устройство с их именем (AirPods держат связь и в кейсе). LE-«тени» наушников отбрасываются.
- Заряд: DualSense/DualShock 4 — HID-отчёт (`SonyPad`), AirPods/Beats — BLE-рассылки Apple (`AirPodsBattery`, слушается только пока они подключены), остальные — свойство Windows `{104EA319…} 2` или GATT Battery Service. Кэш на устройство — 60 с.
- Все вызовы WinRT — через `AwaitOr` с тайм-аутом.

Ниже — описание Windhawk-версии; внутреннее устройство острова (разделы 2–5) в автономной версии то же.

## 0a. Безопасность (standalone 0.9)
- `common/Security.{h,cpp}`: `HardenProcess` (безопасный поиск DLL, политика загрузки образов, ExtensionPointDisable у острова, завершение при повреждении кучи), `JsonLooksSafe` (размер/вложенность), `IsSafeAumid`, `Clip`.
- CMake: /guard:cf, /sdl, /CETCOMPAT, /DEPENDENTLOADFLAG:0x800.
- Launcher: разрешена только первая навигация (встроенный HTML), CSP, сообщения ≤64 КБ, белый список внешних ссылок (`IsAllowedExternalLink`).
- IPC: cbData чётный, ≤256 байт; progress/timer строго проверяются.
- Модель угроз: чужие файлы/данные/сеть; процессы того же пользователя полностью не блокируются.

## 1. Исходный проект

| Параметр | Значение |
|---|---|
| Мод-основа | Dynamic Island for Windows 1.4.0, автор Himanshu (github.com/devcode90), MIT |
| Источник | `ramensoftware/windhawk-mods` → `mods/dynamic-island-for-windows.wh.cpp` |
| Наш форк | `@id dynamic-island-for-windows-custom`, файл `dynamic-island-custom.wh.cpp` |
| Размер | ~16 700 строк после очистки custom.16 (было ~25 900), **один файл** `.wh.cpp` (так устроены все моды Windhawk) |
| Язык | C++20, компилирует сам Windhawk (clang / mingw) |
| Графика | Direct2D + DirectWrite + WIC, DWM |
| Системные API | WinRT (медиа, уведомления, Bluetooth), WASAPI, PDH, WinHTTP, UI Automation, WNF |
| Внешние зависимости | нет; наш форк тоже ничего не добавил |

## 2. Как устроен мод

### 2.1. Запуск
Tool-mod: Windhawk запускает отдельный процесс `windhawk.exe -tool-mod <id>`.
`Wh_ModInit` → `WhTool_ModInit` → `LoadSettings()` + `StartThreads()`.
Mutex `windhawk-tool-mod_<id>` не даёт запустить две копии с одним `@id`. У форка свой `@id`,
поэтому **оригинальный мод нужно отключить**, иначе будет два острова.

### 2.2. Потоки (`StartThreads`)
| Поток | Функция | Назначение |
|---|---|---|
| Render | `RenderThreadProc` | Окно, цикл кадров, пружинные анимации, **вся логика видимости** |
| Media / Audio / Weather / Bluetooth | `...ThreadProc` | Источники данных |
| Stocks | `StocksThreadProc` | Котировки Yahoo Finance для вкладки «Акции» (custom.18) |
| Notification | `NotificationThreadProc` | Опрос `UserNotificationListener` раз в 1 с; в остров попадают только Telegram и Discord |
| ToastWatcher | `ToastWatcher::ThreadProc` | `EVENT_OBJECT_SHOW`: запасной путь чтения баннера Telegram через UIA |
| Mouse | LL-хук | Пробуждение при наведении (клавиатурный хук Caps Lock удалён в custom.16) |
| WNF | `SubscribeDndNotification` | Системный «Не беспокоить» Windows |

### 2.3. Данные
* `Settings g_settings` (+ `g_settingsMutex`, копия через `GetSettingsCopy()`).
* `SharedState g_state` (+ `g_stateMutex`) — «снимки» модулей.
* Источник пишет снимок и вызывает `TriggerNudge()` — это будит отрисовку (в том числе из «парковки»).
* Сохранение между перезапусками: `Wh_SetIntValue / Wh_SetStringValue`.

### 2.4. Что показывать
`ChooseActivities()` → список `IslandKind` по приоритету → `ActivityForKind()` (размер) → `Renderer` (рисование).

### 2.5. Видимость (`RenderThreadProc`)
* Ручное скрытие `g_manuallyHidden` (Ctrl+Alt+D) и **наш «Не беспокоить»** → «парковка» без CPU (`SW_HIDE`, ожидание сообщений).
* Полноэкранный режим: раз в 0,5 с `GameDetector::IsGameOrFullscreen()` (раньше — `IsForegroundFullscreen()`); в «парковке» — раз в 1,5 с.
* `isTransientAlert` — всплывашки, которым разрешено показываться даже в скрытом состоянии.

## 3. Наши изменения (версия 1.4.0-custom.1)

Все новые части помечены в коде комментарием `Custom fork`. Изменено 18 строк оригинала, остальное — добавления.

### 3.1. Модули (новые namespace в одном файле)
| Модуль | Где | Что делает |
|---|---|---|
| `QuietMode` | перед `NotificationThreadProc` | Состояние «Не беспокоить», сохранение (`QuietMode`), подтверждение 1,2 с |
| `GameDetector` | там же | Игра/полноэкран: геометрия окна (как в оригинале) + `SHQueryUserNotificationState` + список exe |
| `TelegramBanner` | там же | Выключает/возвращает баннер Windows для Telegram через реестр (с бэкапом) |
| `TelegramSource` | там же | Узнаёт Telegram-уведомления, разбирает «чат / отправитель / текст», складывает в карточку |
| `TelegramActions` | там же | Клик по карточке → окно Telegram на передний план |
| `TelegramLook` | там же | Инициалы и цвет аватара |
| `QuietToggleLayout` + `QuietButtonHitTest` | рядом с хит-тестами медиаплеера | Положение и нажатие тумблера (вкладка часов и первая вкладка плеера) |
| `Renderer::DrawTelegramNotification`, `Renderer::DrawQuietToggle` | в `Renderer` | Отрисовка |

### 3.2. Точки подключения к оригиналу
| Место | Изменение |
|---|---|
| `NotificationSnapshot` | + `ttl`, `isTelegram`, `sourceAumid`, `telegramLines`, `telegramTotal` |
| `NotificationThreadProc` | Ранний выход при «Не беспокоить»; Telegram и Discord уходят в `TelegramSource::Handle`; **все остальные уведомления игнорируются** (custom.16) |
| `ActivityForKind` | Карточка Telegram шире и растёт на строку за каждое сообщение |
| `DrawNotification` | Удалён в custom.16: вид `Notification` рисуется сразу `DrawTelegramNotification` |
| `DrawIdleDashboard`, `DrawMedia` | Тумблер «Не беспокоить» на первой вкладке (часы / плеер) |
| `WM_LBUTTONUP` | Нажатие на кнопку; клик по карточке Telegram открывает Telegram |
| `WM_HOTKEY`, `ApplyHideShowHotkey` | Горячая клавиша `ID_QUIET_HOTKEY` (Ctrl+Alt+N) |
| `ShowContextMenu` | Пункт «Не беспокоить» (id 95) |
| `RenderThreadProc` | Парковка при «Не беспокоить»; фильтр «в игре только Telegram»; в игре не держат остров: наведение, закрепление, точки микрофона/камеры; один раз поднять поверх игры; облегчённый режим |
| `LoadSettings` | Новые настройки; синхронизация баннера |
| `WhTool_ModInit / Uninit` | Загрузка «Не беспокоить»; возврат баннера Telegram при выключении мода |

### 3.3. Логика функций
**Telegram.** Уведомление считается телеграмовским, если в AUMID или имени приложения есть «telegram».
Разбор текстовых строк: 3+ строки → [чат, отправитель, текст] (группа); 2 → [отправитель, текст]; 1 → [отправитель].
Если сообщение пришло, пока карточка на экране, оно добавляется сверху (до `MaxMessages`), таймер продлевается,
в заголовке появляется «· +N». Клик закрывает карточку и открывает Telegram.

**Два пути получения сообщений Telegram (custom.2).**
1. *Слушатель уведомлений* (`NotificationThreadProc` → `TelegramSource::Handle`). Работает только если Windows разрешила доступ; у непакетных программ вроде Windhawk может не работать.
2. *Окно уведомления* — **удалён в custom.16** вместе с оригинальным перехватом окон через shell hook (`CaptureShellNotification`), который показывал любые уведомления и ложные «уведомления» при закрытии программ.
3. *Показ баннера* (`ToastWatcher`, свой поток, `EVENT_OBJECT_SHOW`). Фильтр: класс CoreWindow / XAML popup / *toast* / *notification*, размер не больше 900×700, процесс ShellExperienceHost / explorer / sihost / ShellHost. Дальше — `TryHandleToastWindow`: через UI Automation читаются текстовые элементы; если среди первых подписей есть «Telegram», создаётся карточка, а баннер закрывается кнопкой «Закрыть» (InvokePattern). Две попытки: через 250 и 650 мс.
`Publish` отбрасывает повтор того же сообщения в течение 10 с.

**Распознавание Telegram в пути 1 (custom.6).** У Telegram Desktop (не-Store) `UserNotification.AppInfo()` бросает исключение → имя и AUMID неизвестны.
Тогда `TelegramSource::IdentifyUnknownApp` (custom.13) перебирает все ключи `Notifications\Settings` и берёт приложение, чья `LastNotificationAddedTime` ближе всего ко времени создания (окно ± 5 с); имя — `FriendlyNameFromAumid`. Так же распознаются Discord и другие программы не из Store.
Если отметок нет вообще — считается Telegram, если запущен процесс Telegram. ⚠ Этот запасной признак может ошибиться, когда одновременно пишет другая не-Store программа.

Слушатель (путь 1): `GetAccessStatus()` → при необходимости `RequestAccessAsync` с ожиданием 10 с; `GetNotificationsAsync` ждёт 5 с. Всё логируется с префиксом `[Notif]`.

**Без дублей Windows.** Для Telegram мод ставит `HKCU\Software\Microsoft\Windows\CurrentVersion\Notifications\Settings\<AUMID>`
→ `ShowBanner = 0`: это штатный переключатель «Показывать баннеры уведомлений». Сообщение тихо уходит в центр уведомлений, оттуда его читает остров.
Исходное значение хранится в `TelegramBannerBackup` и возвращается при выключении настройки или мода.
С custom.2 это делается **только после первого успешного чтения слушателем** (`TelegramBanner::OnListenerReady`). Без работающего слушателя баннер должен оставаться видимым, иначе пути 2 нечего читать.

**Игровой режим.** «В игре» = последний результат `GameDetector` и пользователь не включил показ острова через Ctrl+Alt+D.
Тогда из списка видов остаются только карточки Telegram, а наведение, закрепление и индикаторы микрофона/камеры остров не держат.
«Не беспокоить» Windows в игре для Telegram не учитывается, потому что Windows включает его в играх сама.

**«Не беспокоить».** Тумблер с луной (первая вкладка: часы или плеер), пункт меню или Ctrl+Alt+N. Включение: тумблер переезжает (0,45 с) или, при горячей клавише/меню, сразу (пилюля «Do Not Disturb On» удалена в custom.16);
затем остров плавно сжимается и полностью скрыт, уведомления отбрасываются ещё в потоке уведомлений. Выключение — только горячей клавишей
(или настройками Windhawk: Ctrl+Alt+N можно поменять). Состояние хранится в `QuietMode` и сохраняется после перезагрузки.

## 3a. Удалено в custom.16 (полная очистка)
Удалено из исходного кода целиком (интерфейс, настройки, обработчики, потоки, структуры, пункты меню, переводы):
* Анимации переходов развёрнутого плеера: смена источника, морфинг play/pause, «глубина» кнопок перемотки, «дым» заголовка, смена исполнителя/альбома, переворот обложки.
* Вращение обложки в свёрнутой пилюле.
* Модуль Caps Lock (+ клавиатурный LL-хук `WH_KEYBOARD_LL`).
* Модуль буфера обмена (`AddClipboardFormatListener`, превью текста и картинок).
* Полоса обратного отсчёта (`Behavior.StatusCountdownProgress`).
* Пилюля-статус «Не беспокоить» Windows (вид `IslandKind::DoNotDisturb`). Сам сигнал WNF оставлен: его использует `Telegram.RespectWindowsDnD`.
* Всё про тексты песен и караоке: LRCLIB, дисковый кэш, вкладка Lyrics, иммерсивный режим, строка в свёрнутой пилюле, сдвиг синхронизации, подменю.
* Quick Lookup (Wiktionary / Wikipedia / dictionaryapi.dev, горячая клавиша Ctrl+Alt+Space).
* File Tray (перетаскивание файлов, OLE drag-and-drop, `WM_DROPFILES`, хранение списка).
* Общие уведомления Windows: показ любых других приложений в острове, перехват окон через shell hook (`RegisterShellHookWindow`, `HSHELL_WINDOWCREATED`) — он же давал «уведомления» при закрытии программ, настройка `Modules.NotificationRespectDnD`, подбор иконки приложения (`FindAppIconByName`).
Вкладки развёрнутого острова теперь: плеер (если играет) / часы → календарь → погода → монитор железа → акции (custom.18).

## 3b. Liquid Glass (custom.17)
`Renderer::DrawLiquidGlass` вызывается из `DrawPillSurface`, когда `settings.liquidGlass`. Слои: линза по краю → блик сверху/отсвет снизу (вертикальный градиент `glassSheenGrad_`) → кромка (ровная + угловые блики через `glassRimGrad_`, верх-лево 0,70, низ-право 0,40, внутренняя 0,22) → тёмная линия 0,14. Заливка: tinted 0,55→0,12, clear 0,62→0,22 по `LiquidGlassClarity`. DWM: `DwmBackdrop::Sync` для clear прячет окна размытия; для tinted — обычное размытие с тонировкой ×0,08. Настоящего преломления нет (нет доступа к пикселям под окном).

## 3c. Вкладка «Акции» (custom.18)
Всегда последняя вкладка (`ActiveTabCount` +1, id вкладки 4 в обоих списках). Данные: `Stocks::FetchQuote` → Yahoo `v8/finance/chart` (query1, затем query2), браузерный User-Agent. Из ответа берутся `regularMarketPrice`, `chartPreviousClose` (или `previousClose`), `currency`, массив `close` (null пропускаются, до 120 точек). Изменение % = к цене закрытия перед периодом графика. Рендер: `DrawStocksDashboard` отмечает `g_stocksLastViewed` и при старых данных будит поток (`g_stocksWakeEvent`, `g_stocksForceFetch`). Список (50 акций + SpaceX + индекс S&P 500, всего 52) — `kStockCatalog`; индексы (тикер с `^`) показываются без валюты; в YAML он повторён в 6 выпадающих списках (генерируется скриптом, при изменении менять оба места).

## 4. Технические ограничения
1. Один файл — «модульность» реализована через namespace и минимальные точки подключения.
2. Обновления Windhawk для оригинала форк не трогают; новые версии оригинала переносим вручную.
3. Слушатель уведомлений только читает их. Подавление баннера держится на настройке Windows (п. 3.3). ⚠ Не проверено, что Windows подхватывает
   изменение реестра без перезапуска. Самое первое уведомление Telegram, у которого ещё нет ключа в реестре, может показать баннер один раз.
4a. Если слушатель не работает, сообщения Telegram в **играх** не придут: Windows сама не показывает баннеры во время игр, поэтому пути 2 нечего читать.
4. Фото профиля через API слушателя недоступно (только текст) — показываются инициалы.
5. Открыть конкретный чат нельзя: открывается окно Telegram.
6. Поверх игры в **эксклюзивном** полноэкранном режиме окно показать нельзя. Нужен «оконный без рамки» / «полноэкранный (оптимизированный)».
7. Уведомления опрашиваются раз в 1 с (как в оригинале) → задержка до 1 с.
8. Telegram API / TDLib не используются (безопасность аккаунта).
9. Собрать здесь нельзя (нет WinRT-заголовков). Проверено: весь файл проходит компилятор mingw (clang 20) без новых ошибок, кроме тех, что вызваны заглушками WinRT (сравнение со списком ошибок до очистки).
   Код с WinRT проверен только вручную. Окончательная проверка — компиляция в Windhawk.
10. Подписи в UI на русском написаны прямо в коде, без системы переводов оригинала (`Loc`).

## 5. Настройки форка
| Ключ | По умолчанию | Назначение |
|---|---|---|
| `Telegram.Enabled` | вкл | Карточки Telegram |
| `Telegram.SuppressWindowsBanner` | вкл | Выключить баннер Windows для Telegram (обратимо) |
| `Telegram.DisplaySeconds` | 6 | Время показа (2–60) |
| `Telegram.MaxMessages` | 3 | Сообщений в карточке (1–5) |
| `Telegram.ShowMessageText` | вкл | Иначе «Новое сообщение» |
| `Telegram.RespectWindowsDnD` | вкл | Учитывать DnD Windows вне игр |
| `Telegram.ClearFromNotificationCenter` | выкл | Удалять из центра уведомлений |
| `Telegram.DebugLog` | выкл | Лог структуры уведомлений (с текстом!) |
| `Discord.Enabled` | вкл | Карточки Discord (как у Telegram) |
| `Discord.ShowOverGames` | вкл | Discord поверх игр |
| `Discord.SuppressWindowsBanner` | вкл | Выключить баннер Windows для Discord (обратимо) |
| `Themes.BackdropMaterial = liquidglass` | — | Материал Liquid Glass (custom.17) |
| `Themes.LiquidGlassStyle` | tinted | tinted — с размытием Windows, clear — без размытия |
| `Themes.LiquidGlassClarity` | 60 | Прозрачность стекла 0–100 |
| `Themes.LiquidGlassShine` | 100 | Блики и края 0–200 |
| `Stocks.Enabled` | вкл | Вкладка «Акции» |
| `Stocks.Range` | 1d | Период графика |
| `Stocks.RefreshMinutes` | 2 | Частота обновления, пока вкладку смотрят |
| `Stocks.Stock1`…`Stock6` | AAPL, MSFT, NVDA, GOOGL, AMZN, TSLA | Избранные акции |
| `GameMode.SmartGameMode` | вкл | В игре только Telegram (и Discord, если разрешено) |
| `GameMode.ShowOtherAlertsInGame` | выкл | Вернуть поведение оригинала в играх |
| `GameMode.DetectD3DFullscreen` | вкл | Учитывать сигнал Windows о полноэкранной игре |
| `GameMode.GameProcesses` | "" | Доп. exe-игры через запятую |
| `GameMode.LightweightOverlay` | вкл | Без размытия, ≤60 FPS поверх игр |
| `QuietMode.ShowButton` | вкл | Кнопка в острове |
| `QuietMode.HotkeyEnabled` / `HotkeyModifiers` / `HotkeyKey` | вкл / Ctrl+Alt / N | Горячая клавиша |
| Сохраняемые значения | | `QuietMode` (0/1), `TelegramBannerBackup` |

Зависимость от оригинальных настроек: игровой режим работает, только если включено `Behavior.AutoHideFullscreen` («Hide on full screen»).

## 6. Установка
1. Windhawk → оригинальный «Dynamic Island for Windows» → **Отключить**.
2. Windhawk → «Create a new mod» (или «Fork» у оригинала) → заменить весь текст содержимым `dynamic-island-custom.wh.cpp` → **Compile Mod**.
3. Если компиляция выдала ошибки — скопировать их целиком и прислать.
4. Windows: Параметры → Конфиденциальность → Уведомления → «Разрешить приложениям доступ к уведомлениям» = Вкл.
5. Telegram Desktop: Настройки → Уведомления и звуки → включить системные (Windows) уведомления.

## 7. Как проверять
См. ответ к версии 1.4.0-custom.1 и раздел «Проверка» в `TODO.md`. Логи: Windhawk → мод → Details/Log.
