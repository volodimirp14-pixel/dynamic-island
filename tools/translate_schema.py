"""Russian names, descriptions and option labels for the settings that came
from the upstream (English) mod. Applied once to src/common/settings_schema.json."""
import json
p = "src/common/settings_schema.json"
d = json.load(open(p, encoding="utf-8"))

G = {"Appearance": "Положение и размер", "Behavior": "Поведение и видимость", "Animations": "Анимации и плавность",
     "Themes": "Цвета и материал", "Indicators": "Индикаторы микрофона и камеры", "Modules": "Модули",
     "Telegram": "Telegram", "Discord": "Discord", "Stocks": "Акции", "GameMode": "Игровой режим",
     "QuietMode": "Не беспокоить", "Shortcuts": "Горячие клавиши"}

# key: (name, description or None, [option labels in order] or None)
T = {
"Appearance.Position": ("Положение на экране", None, ["Сверху по центру", "Сверху слева", "Сверху справа", "Снизу по центру", "Снизу слева", "Снизу справа"]),
"Appearance.TargetMonitor": ("Монитор", "Если выбранного монитора нет, остров появится на основном.", ["Основной монитор", "Дисплей 1", "Дисплей 2", "Дисплей 3", "Дисплей 4", "Дисплей 5", "Следовать за мышью (активный монитор)"]),
"Appearance.OffsetX": ("Сдвиг по горизонтали (px)", "Отрицательное значение — влево, положительное — вправо.", None),
"Appearance.OffsetY": ("Сдвиг по вертикали (px)", "Отрицательное значение — вверх, положительное — вниз.", None),
"Appearance.SeparateExpandedOffsetY": ("Отдельный сдвиг по вертикали для развёрнутого острова", None, None),
"Appearance.OffsetYExpanded": ("Сдвиг по вертикали в развёрнутом виде (px)", "Используется, только если включён пункт выше.", None),
"Appearance.BorderMergedMode": ("Слияние с краем экрана", "Остров прилегает к краю экрана без отступа.", None),
"Appearance.ShapeStyle": ("Форма острова", None, ["Пилюля (как у Apple)", "Windows 11 (скруглённый прямоугольник)", "Вырез macOS (прилегает к верхнему краю)"]),
"Appearance.ProgressStyle": ("Вид полосы прогресса трека", None, ["Тонкая полоса", "Волна", "Плавная волна (медленнее)", "Толстая полоса с ползунком"]),
"Appearance.SpectrumStyle": ("Вид эквалайзера", None, ["Классические столбики", "Пиковая матрица", "Плазменная нить", "Пульсирующий шар"]),
"Appearance.SizeScale": ("Размер острова", None, None),
"Appearance.AutoDpiScale": ("Учитывать масштаб экрана Windows", "Остров увеличивается вместе с масштабом 125–200 %.", None),
"Behavior.AlwaysOnTop": ("Поверх всех окон", None, None),
"Behavior.ExpandOnHover": ("Раскрывать при наведении", "Если выключено — раскрывается по щелчку.", None),
"Behavior.AutoHideIdleSeconds": ("Автоматически скрывать остров", None, ["Сразу", "Никогда (по умолчанию)", "Через 5 секунд", "Через 10 секунд", "Через 30 секунд", "Через 60 секунд"]),
"Behavior.AutoHideFullscreen": ("Скрывать в полноэкранном режиме", "Нужно для игрового режима.", None),
"Behavior.UnhideOnHover": ("Показывать скрытый остров при наведении", None, None),
"Animations.TargetFPS": ("Частота кадров анимаций", "Больше — плавнее, но больше нагрузка.", ["Авто (как у монитора)", "60 FPS (экономно)", "90 FPS", "120 FPS", "144 FPS", "165 FPS", "240 FPS", "360 FPS (очень плавно)", "500 FPS (без ограничения)"]),
"Animations.AnimationStyle": ("Характер анимации", None, ["Плавная (без пружинистости)", "Стандартная (пружина как у Apple)", "Упругая", "Резкая (быстро останавливается)"]),
"Animations.AnimationSpeed": ("Скорость анимации", None, ["Очень медленно (0,5×)", "Медленно (0,75×)", "Обычно (1×)", "Быстро (1,35×)", "Очень быстро (1,65×)", "Максимально быстро (2×)"]),
"Themes.ThemePreset": ("Тема", None, ["Obsidian — чёрная (по умолчанию)", "Graphite — тёмная Windows 11", "Slate — сине-серая", "Nord — палитра Nord", "Evergreen — тёмно-зелёная", "Espresso — тёплая коричневая", "Plum — приглушённая сиреневая", "Porcelain — светлая", "Свои цвета (поля ниже)"]),
"Themes.PillOpacity": ("Непрозрачность острова (35–100)", "Меньше — прозрачнее.", None),
"Themes.TintIntensity": ("Затемнение фона (0–100)", None, None),
"Themes.BackdropMaterial": ("Материал фона", "Настоящее размытие Windows за островом. Для «Размытие», «Акрил» и Liquid Glass «С тонировкой» нужны «Эффекты прозрачности» Windows (Параметры → Персонализация → Цвета).", ["Выключено (сплошной фон)", "Размытие", "Акрил (матовое стекло)", "Liquid Glass (как в iOS)"]),
"Themes.BackdropFillOpacity": ("Плотность фона поверх размытия (0–100)", "Меньше — сильнее видно размытый рабочий стол. Не влияет на Liquid Glass.", None),
"Themes.BackdropTint": ("Тонировка акрила (0–100)", None, None),
"Themes.MaterialDepth": ("Объём (затенение и подсветка акцентом)", None, None),
"Themes.DropShadow": ("Мягкая тень под островом", None, None),
"Themes.AccentBloom": ("Сила подсветки акцентом (0–200)", "Цветное свечение сверху острова, по умолчанию берётся из обложки.", None),
"Themes.TextScale": ("Размер текста (70–160 %)", None, None),
"Themes.AccentColorMode": ("Цвет акцента", None, ["Авто, из обложки", "Как в Windows", "Свой цвет (ниже)"]),
"Themes.CustomAccentHex": ("Свой цвет акцента", "Например #0A84FF.", None),
"Themes.CalendarAccent": ("Цвет акцента календаря", None, ["Как у острова", "Как в Windows"]),
"Themes.ClockAccentGlow": ("Свечение за часами", None, None),
"Themes.FontFamily": ("Шрифт", "Название шрифта Windows, например Segoe UI Variable. Пусто — по умолчанию.", None),
"Themes.ContourBorderMode": ("Рамка острова", None, ["Обычная", "Авто (цвет обложки)", "Без рамки"]),
"Themes.ContourBorderHex": ("Цвет рамки", None, None),
"Themes.PillBgColor": ("Цвет фона острова", "Для темы «Свои цвета». Формат #RRGGBB или #RRGGBBAA (с прозрачностью).", None),
"Themes.TextPrimaryColor": ("Цвет основного текста", "Для темы «Свои цвета».", None),
"Themes.TextSecondaryColor": ("Цвет второстепенного текста", "Для темы «Свои цвета».", None),
"Indicators.PrivacyDots": ("Показывать индикаторы микрофона и камеры", None, None),
"Indicators.PrivacyDotsMic": ("Индикатор микрофона (оранжевая точка)", None, None),
"Indicators.PrivacyDotsMicHex": ("Цвет индикатора микрофона", None, None),
"Indicators.PrivacyDotsCam": ("Индикатор камеры (зелёная точка)", None, None),
"Indicators.PrivacyDotsCamHex": ("Цвет индикатора камеры", None, None),
"Modules.Media": ("Музыкальный плеер", None, None),
"Modules.MediaAutoExpand": ("Раскрывать при смене трека", None, None),
"Modules.MediaPillClock": ("Часы в свёрнутом плеере", None, None),
"Modules.Volume": ("Ползунок громкости", None, None),
"Modules.Brightness": ("Ползунок яркости", None, None),
"Modules.Battery": ("Батарея", "Предупреждение о низком заряде и зарядке.", None),
"Modules.BluetoothIndicator": ("Подключение Bluetooth-устройств", None, None),
"Modules.BluetoothShowBattery": ("Заряд Bluetooth-устройств", None, None),
"Modules.Progress": ("Кольцо прогресса", "Включается командой DynamicIsland.exe --progress N.", None),
"Modules.TimerModule": ("Таймер фокусировки", "Запускается из меню правой кнопки.", None),
"Modules.HardwareMonitorModule": ("Вкладка «Монитор железа»", None, None),
"Modules.GameOverlay": ("Игровой оверлей", "Вместо часов — FPS, загрузка процессора и памяти.", None),
"Modules.ShowMetricText": ("Подписи у показателей", None, None),
"Modules.Weather": ("Погода", None, None),
"Modules.WeatherCity": ("Город для погоды", "Например Amsterdam. Пусто — определяется автоматически по IP.", None),
"Modules.WeatherFahrenheit": ("Градусы Фаренгейта", None, None),
"Modules.Language": ("Язык подписей острова", None, ["Как в Windows", "English", "Français", "Español", "Deutsch", "Português", "Italiano", "Русский", "Türkçe", "हिन्दी", "简体中文", "日本語", "한국어"]),
"Modules.ClockFormat": ("Формат времени", None, ["Как в Windows", "12 часов (3:07 PM)", "24 часа (15:07)"]),
"Modules.ShowSeconds": ("Показывать секунды", None, None),
"Modules.DateFormat": ("Свой формат даты", "Например dd.MM.yyyy или dddd, d MMMM. Пусто — по умолчанию.", None),
"Modules.DateFirst": ("Дата над временем", None, None),
"Modules.FirstDayOfWeek": ("Первый день недели", None, ["Как в Windows", "Понедельник", "Воскресенье"]),
"Modules.MediaExpandBlocklist": ("Не раскрывать плеер для программ", "Названия процессов через запятую, например chrome.exe, msedge.exe.", None),
"Modules.GameOverlayShowFps": ("Оверлей: FPS", None, None),
"Modules.GameOverlayShowCpu": ("Оверлей: процессор", None, None),
"Modules.GameOverlayShowGpu": ("Оверлей: видеокарта", None, None),
"Modules.GameOverlayShowRam": ("Оверлей: память", None, None),
"Modules.GameOverlayShowDisk": ("Оверлей: диск", None, None),
"Modules.GameOverlayCompact": ("Оверлей: компактный размер", None, None),
"Shortcuts.HideShowHotkeyEnabled": ("Горячая клавиша «скрыть/показать»", "Единственный способ вернуть скрытый остров, не выключайте без необходимости.", None),
"Shortcuts.HideShowModifiers": ("Модификаторы", None, None),
"Shortcuts.HideShowKey": ("Клавиша (A–Z или 0–9)", None, None),
}

for g in d["groups"]:
    g["name"] = G.get(g["key"], g["name"])
done = 0
for s in d["settings"]:
    t = T.get(s["key"])
    if not t:
        continue
    name, desc, labels = t
    s["name"] = name
    if desc is not None:
        s["description"] = desc
    elif "description" in s and not any("а" <= ch <= "я" for ch in s["description"].lower()):
        s.pop("description")  # the long English upstream text; the name says enough
    if labels:
        if len(labels) != len(s["options"]):
            raise SystemExit(f"{s['key']}: {len(labels)} labels for {len(s['options'])} options")
        for o, lab in zip(s["options"], labels):
            o["label"] = lab
    done += 1
json.dump(d, open(p, "w", encoding="utf-8"), ensure_ascii=False, indent=2)
print("translated", done)
