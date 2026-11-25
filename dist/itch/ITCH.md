# Страница на itch.io — что заполнить

Создание страницы: https://itch.io/game/new (нужен ваш аккаунт itch.io).

| Поле | Что указать |
|---|---|
| Title | Dynamic Island for Windows |
| Project URL | `dynamic-island` (адрес будет `<ваш-ник>.itch.io/dynamic-island`) |
| Short description or tagline | iPhone-style Dynamic Island for Windows: music, notifications, Bluetooth, games |
| Classification | **Tools** |
| Kind of project | Downloadable |
| Release status | Released |
| Pricing | **No payments** (программа бесплатная; донаты — ссылками в описании) |
| Uploads | `DynamicIsland-1.0.0-win-x64.zip` из GitHub Release, отметить **Windows** |
| Description | текст ниже |
| Genre | (пусто) |
| Tags | `windows`, `desktop`, `utility`, `music`, `notifications`, `widget`, `customization` |
| Cover image | `dist/itch/cover.png` (630×500) |
| Screenshots | 3–5 снимков острова на вашем экране (плеер, уведомление, вкладки) |
| Visibility | Public (после проверки — сначала можно Draft) |

Правила itch.io: программы разрешены (раздел Tools). Метки и раздел должны соответствовать программе — не добавлять игровые жанры и чужие движки.

## Автоматическая загрузка новых версий (необязательно)
1. На itch.io: https://itch.io/user/settings/api-keys → «Generate new API key».
2. На GitHub: Settings → Secrets and variables → Actions:
   - **Secrets** → New: `BUTLER_API_KEY` = ключ из шага 1;
   - **Variables** → New: `ITCH_TARGET` = `<ваш-ник>/dynamic-island:windows`.
3. Дальше каждый выпуск (новая версия в Version.h) сам загружается и на GitHub, и на itch.io.

---

## Description (English)

**Dynamic Island for Windows** brings the iPhone-style island to the top of your screen.

- 🎵 **Music** — cover art, live spectrum, seek bar and controls for Spotify, Apple Music, YouTube in the browser and any player that shows in Windows media controls
- 💬 **Telegram & Discord** — messages appear in the island (read from Windows notifications; no account access needed)
- 🎧 **Bluetooth** — connect / disconnect pop-ups and battery level for headphones (incl. AirPods) and gamepads (incl. DualSense)
- 🎮 **Game mode** — hides in full-screen games; optional FPS / CPU / RAM overlay and hardware monitor
- ☀️ Weather, 📈 stocks, ⏱ focus timer, 🖥 screen time, volume, brightness, battery, Do Not Disturb
- 🎨 Liquid Glass and acrylic looks, accent colours, themes; move the island with the mouse (hold half a second)
- ⚙️ Settings app (Launcher) with autostart

**Requirements:** Windows 10 or 11, 64-bit. Unzip anywhere and run `DynamicIslandLauncher.exe`.
Windows SmartScreen may warn on first run (the app isn't code-signed): *More info → Run anyway*. Open source: https://github.com/volodimirp14-pixel/dynamic-island

**Free, with every feature.** If you like it, support development:
Donate (EU / world): https://donatello.to/DynamicIsland · Donate (RU / CIS): https://donatepay.ru/don/1545186

## Описание (русский)

**Dynamic Island для Windows** — остров в стиле iPhone вверху экрана.

- 🎵 **Музыка** — обложка, спектр, перемотка и кнопки для Spotify, Apple Music, YouTube в браузере и любого плеера из медиа-управления Windows
- 💬 **Telegram и Discord** — сообщения прямо в острове (из уведомлений Windows, доступ к аккаунтам не нужен)
- 🎧 **Bluetooth** — подключение устройств и заряд наушников (в том числе AirPods) и геймпадов (в том числе DualSense)
- 🎮 **Игровой режим** — прячется в полноэкранных играх; оверлей FPS / CPU / RAM и монитор железа
- ☀️ Погода, 📈 акции, ⏱ таймер фокусировки, 🖥 экранное время, громкость, яркость, батарея, «Не беспокоить»
- 🎨 Liquid Glass и акрил, цвет акцента, темы; остров можно передвигать мышью (зажать на полсекунды)
- ⚙️ Окно настроек (Launcher) с автозапуском

**Требования:** Windows 10 или 11, 64 бит. Распакуйте в любую папку и запустите `DynamicIslandLauncher.exe`.
SmartScreen может предупредить при первом запуске (программа не подписана сертификатом): «Подробнее» → «Выполнить в любом случае». Исходный код: https://github.com/volodimirp14-pixel/dynamic-island

**Бесплатно и со всеми функциями.** Поддержать разработку:
Донат EU: https://donatello.to/DynamicIsland · Донат RU: https://donatepay.ru/don/1545186
