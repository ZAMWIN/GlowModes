# MODLOG — Per-Gamemode Glow (Geometry Dash / Geode)

Цель: glow настраивается независимо для каждой формы игрока (куб, корабль, шар, UFO,
волна/ dart, робот, паук, свинг, джетпак): цвет, яркость, режим смешивания, вкл/выкл.
Настройки одной формы не влияют на другие.

## 0. Intake

- Игра: Geometry Dash (Steam appid 322170), установлена.
- Путь: `<...>\steamapps\common\Geometry Dash`
- Идея: per-gamemode glow (цвет/яркость/blending/пульсация) вместо глобального.
- Definition of done: мод собран в `.geode`, загружается в игре, каждая форма использует
  свой профиль; поведение проверено в игре (скриншот/видео).

## 1. Recon (факты, не догадки)

### Игра / движок
- Движок: **cocos2d-x** (C++), `libcocos2d.dll`, exe x64, anti-cheat нет.
- Версия GD: **2.209** (Steam news: "Update 2.209", 2026-10-15). PE timestamp exe: 2026-10-17.
- Steam build id 26002420, depot 322171.
- Лоадер: **Geode отсутствует** (нет `Geode.dll`, нет папки `geode/` в игре).
- Последний Geode: **v5.11.2** (2026-10-15), совместим с 2.209 (в форвард-компат режиме;
  релиз "still primarily for 2.2081").

### Тулчейн на машине
- **Есть MSVC**: `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools`,
  toolset 14.51.36231, `cl.exe` в `...\VC\Tools\MSVC\14.51.36231\bin\Hostx64\x64\cl.exe`,
  `vcvars64.bat` в `...\VC\Auxiliary\Build\vcvars64.bat`.
- **НЕТ CMake / Ninja** (есть `pip` Python 3.14, winget, choco).
- Есть git, node, dotnet. Свободно на C: 58.6 GB.
- Windows SDK Include пусто при первом взгляде — УТОЧНИТЬ при первой сборке.

### Имена glow-спрайтов игрока (ПОДТВЕРЖДЕНО на файлах игры)
Гипотеза из ТЗ (`{gamemode}_{iconID}_glow_001.png` в `GJ_GameSheetGlow`) — **частично неверна**:
- `GJ_GameSheetGlow.plist` (209 кадров) содержит glow **объектов** (blockOutline*, blade*,
  blackCogwheel*, spiderRing/spiderBump), а НЕ glow игрока.
- Glow игрока лежит в **`Resources/icons/<prefix>_<id>.png/.plist`** — по одной текстурной
  странице на иконку. Кадры в ней:

| Форма | Префикс | Кадры в `<prefix>_01.plist` |
|---|---|---|
| Куб | `player` | player_01_001.png, player_01_2_001.png, **player_01_glow_001.png** |
| Шар | `player_ball` | player_ball_01_001.png, player_ball_01_2_001.png, **player_ball_01_glow_001.png** |
| Корабль | `ship` | ship_01_001.png, ship_01_2_001.png, **ship_01_glow_001.png** |
| UFO | `bird` | bird_01_001/2/3_001.png, **bird_01_glow_001.png** |
| Волна | `dart` | dart_01_001.png, dart_01_2_001.png, **dart_01_glow_001.png** |
| Робот | `robot` | robot_01_01..04_001.png, robot_01_01..04_2_001.png, **robot_01_0N_glow_001.png** (4 кадра) |
| Паук | `spider` | spider_01_01..04_001.png (+`_extra_`), **spider_01_0N_glow_001.png** (4 кадра) |
| Свинг | `swing` | swing_01_001.png (+`_extra_`), **swing_01_glow_001.png** |
| Джетпак | `jetpack` | jetpack_01_001.png, jetpack_01_2_001.png, **jetpack_01_glow_001.png** |

Итого шаблон: `{prefix}_{id}_glow_001.png` (+ у робота/паука суффикс-анимация `_{n}`).

### API PlayerObject (Geode bindings 2.209) — точки внедрения
Члены:
- `bool m_hasGlow;`
- `cocos2d::CCSprite* m_iconGlow;`   (glow иконки: куб/шар/UFO/волна/робот/паук/свинг)
- `cocos2d::CCSprite* m_vehicleGlow;` (glow транспорта: корабль / джетпак)
- `bool m_hasCustomGlowColor;`
- `cocos2d::ccColor3B m_glowColor;`
- флаги режима: `m_isShip, m_isBird(UFO), m_isBall, m_isDart(волна), m_isRobot, m_isSpider, m_isSwing`
  (куб = все флаги false; джетпак — отдельный признак, уточнить)

Методы и НАЛИЧИЕ win-адреса (Geode хукает только то, у чего есть адрес на Windows):

| Метод | win | решение |
|---|---|---|
| `update(float)` | **0x4e3930** | ✅ главный кандидат — пер-кадровое применение |
| `updatePlayerArt()` | **0x4f33e0** | ✅ вызывается при смене арта/режима |
| `updatePlayerFrame(int)` | **0x4f8730** | ✅ (тот же адрес, что updatePlayerBirdFrame — уточнить) |
| `switchedToMode(GameObjectType)` | **0x4f6900** | ✅ смена режима |
| `toggleFlyMode/BirdMode/DartMode/RollMode/RobotMode/SpiderMode/SwingMode` | ✅ win | ✅ |
| `updateGlowColor()` | — | ❌ нет win-адреса, НЕ хукнуть |
| `updatePlayerGlow()` | — | ❌ нет win-адреса, НЕ хукнуть |
| `enableCustomGlowColor(ccColor3B const&)` | **win inline** | ✅ вызываемый (реализация Geode ниже) |
| `disableCustomGlowColor()` | **win inline** | ✅ вызываемый |
| `setColor(ccColor3B const&)` (virtual) | **0x4f7dc0** | ✅ |

Inline-реализации (Geode, ground truth):
```cpp
void PlayerObject::enableCustomGlowColor(cocos2d::ccColor3B const& color) {
    m_hasCustomGlowColor = true;
    m_glowColor = color;
}
void PlayerObject::disableCustomGlowColor() {
    m_hasCustomGlowColor = false;
}
```

### GameManager (global glow)
```cpp
bool getPlayerGlow();                 // "Glow Enabled" глобальный тумблер
int  getPlayerGlowColor();            // id цвета (не сам цвет)
void setPlayerGlow(bool);
geode::SeedValueRSV m_playerGlowColor;
bool m_playerGlow;
bool m_addGlow;
```

## 2. Выбор маршрута

**Маршрут: Geode SDK (loader API + `$modify` хуки).** Не нативные MinHook-прокси:
сообщество GD живёт на Geode, лоадер даёт bindings, matjson-конфиг, Popup-UI и совместимость
с другими визуальными модами (Blending Glow, Custom Gamemode Colors и т.п.).

**Минимальный хук (не переписываем рендер):**
1. `PlayerObject::update(float)` (post) — каждый кадр для каждого игрока применяем профиль
   текущего режима (цвет/opacity/blendFunc к `m_iconGlow`/`m_vehicleGlow`). Дёшево (2 спрайта),
   самовосстанавливается после вмешательства ванильного кода.
2. `PlayerObject::updatePlayerArt()` (post) — применяем сразу при смене иконки/режима, чтобы
   не было одного кадра со старым glow.
3. Отключаемо: если профиль `enabled:false` или глобальный glow выключен → не вмешиваемся
   (ванильное поведение).

**Почему не «подмена m_glowColor на время рендера»:** глобальный `GameManager::m_playerGlowColor`
трогать не будем (это протекало бы в UI/меню). Работаем только с полями конкретного PlayerObject.

## 3. Lab setup
- Backup сейвов GD: `%LOCALAPPDATA%\Geometry Dash` (уточнить путь; на этой машине папки нет →
  игра ещё не запускалась? проверить). Сделать `um backup` перед первым мод-запуском.
- Проект мода: отдельная папка с исходниками вне папки игры.
- Папку игры не коммитить; декомпиляцию/выгрузки хранить отдельно.

## 4. План реализации (см. README.md)
- `mod.json`, `CMakeLists.txt`, `src/main.cpp`, `README.md`.
- Конфиг `per_gamemode_glow.json` в `Mod::get()->getConfigDir()` (создаётся с дефолтами).
- Параметры на форму: `enabled, color (#RRGGBB), brightness (0..~2), blending (normal|additive),
  pulse (0 = выкл; иначе период в сек)`.
- Опционально: Geode Popup для переключения профилей.

## 5. Открытые вопросы / TODO
- [ ] Точный путь сейвов GD на этой машине.
- [ ] Признак джетпак-режима в PlayerObject (какой член/икон-тип).
- [x] Есть ли Windows SDK (для сборки) — ДА, 10.0.26100.0.
- [x] Установка Geode loader — сделано в lab-копию.

## 5a. Тестовый уровень: ID порталов (ПОДТВЕРЖДЕНО по именованному списку
`flowvix.github.io/gd-info-explorer/ids`, credit Alphalaneous + community)

| Режим | Object ID | Название в списке |
|---|---|---|
| Куб | 12 | Cube Portal |
| Корабль | 13 | Ship Portal |
| Шар | 47 | Ball Portal |
| UFO | 111 | UFO Portal |
| Волна | 660 | Wave Portal |
| Робот | 745 | Robot Portal |
| Паук | 1331 | Spider Portal |
| Свинг | 1933 | Swing Portal |

Служебные: 10/11 гравитационные порталы, 45/46 зеркала, 99/101 размер,
200/201/202/203/1334 порталы скорости, 286/287 дуэль, 747 телепорт,
31 стартовая позиция, 899 Color Trigger, 914 Text.

Формат объекта в level string: `1,<id>,2,<x>,3,<y>;` (1=id, 2=x, 3=y, 4=flip,
5=rotation, 6=scale …). Минимальный уровень: просто список объектов.
Хранится в `CCLocalLevels.dat` как plist GD-формата (компактные теги `<d>/<k>/<s>/<t>/<i>/<r>/<a>`),
поле `k4` = base64(zlib(levelString)). Проверить эмпирически для 2.209.

Ещё подтверждено: `objectDefinitions.plist` — только анимации по режимам (Robot/Spider/...),
НЕ ID объектов. Кадры порталов: `GJ_GameSheet02/04.plist`, имена `portal_NN_{front,back,extra}.png`.

## 6. Версия игры = GD 2.2091 (а не 2.209) и что из этого следует

**Как определили (надёжно, без догадок).** Geode в forward-compat пишет версию как
`v1792203143` — это сырой PE TimeDateStamp exe (0x6AD2D987, 17.10.2026), которого нет в
`gdTimestampMap.hpp` у Geode 5.11.2. Поэтому версию взяли механически: в x64 PE есть таблица
`.pdata` со стартовыми адресами **всех** функций; посчитали, какая доля адресов `win 0x...` из
данных каждой версии попадает в реальные начала функций этого exe:

| Версия биндингов | Совпадений |
|---|---|
| **2.2091** | **4437/5299 = 83.7 %** |
| 2.209 | 323/4976 = 6.5 % |
| 2.2081 | 126/5455 = 2.3 % |

Остальные ~16 % — адреса DLL (libcocos2d/fmod) и функции без записи в `.pdata`, а не
расхождения. Скрипт: `lab-tools/detect_version.py`.

**Почему это критично.** Сгенерированный код биндингов вызывает игру по адресу-константе,
вшитой на этапе компиляции, например:

```cpp
static auto func = wrapFunction<FunctionType>(base::get() + 0x388d80, ...);   // 2.2081
```

`0x388d80` (`PlayerObject::update` в 2.2081) в exe 2.2091 **не является началом функции**
(проверено по `.pdata`). То есть мод, собранный под 2.2081, воткнул бы хук в середину чужой
функции. В 2.2091 `PlayerObject::update` = `0x4e6fc0`, `updatePlayerArt` = `0x4f6bb0`.

**Что делает сам Geode.** `ModMetadata::checkGameVersion()` в режиме forward-compat
**отклоняет** моды с целевой версией `<= GEODE_GD_VERSION` (2.2081) и пропускает моды с
целевой версией **новее** — то есть сборка под 2.2091 это и есть штатный механизм для
нашей ситуации, а `"win": "*"` ставить нельзя (мод грузился бы там, где адреса неверны).
Поэтому в `mod.json`: `"gd": { "win": "2.2091" }`.

**Что понадобилось, чтобы собраться под 2.2091:**
1. `CMakeLists.txt` задаёт `GEODE_GD_VERSION=2.2091` **и** `GEODE_COMP_GD_VERSION=22091`.
   SDK ставит обе переменные только внутри одного `if (NOT DEFINED GEODE_GD_VERSION)`,
   а биндинги используют `#if GEODE_COMP_GD_VERSION < 22080` → с пустым макросом MSVC
   падает с `C1017` в `bindings/include/Geode/Enums.hpp`.
2. Поменялись сигнатуры экспортов cocos2d (это и была ошибка `ERROR_PROC_NOT_FOUND`):
   `DS_Dictionary::loadRootSubDictFromCompressedFile(char const*, bool)`,
   `DS_Dictionary::saveRootSubDictToCompressedFile(char const*, bool)`,
   `CCTouchDispatcher::registerForcePrio(CCObject*, int, int)` — в 2.2091 у них есть
   лишний параметр. Биндинги 2.2081 тянули их как прямые импорты из `libcocos2d.dll` →
   DLL не загружалась. Биндинги 2.2091 линкуются с правильными сигнатурами.
3. Сами биндинги 2.2091 релизным SDK 5.11.2 не собираются вообще — подробности и решение
   в §7 (из v6 берутся только описания cocos2d-ABI 2.2091, SDK остаётся релизный).

**Инструменты** (личная папка лабораторных скриптов): `detect_version.py` (версия игры по
`.pdata`), `check_imports.py` (неразрешённые импорты мода против экспортов DLL игры),
`check_missing_includes.py` (каких `Geode/...` заголовков не хватает биндингам),
`layout_diff_all.py` (сравнение списков полей классов между версиями биндингов),
`gdsave.py` (сейвы/уровни).

## 7. Сборка под 2.2091: релизный SDK + описания cocos2d из ветки `v6`

**Что выяснилось.** Набор биндингов 2.2091 в `geode-sdk/bindings` появился раньше, чем
поддержка 2.2091 в SDK: в релизе 5.11.2 (ветка `main`) в `loader/include` нет заголовков двух
новых cocos2d-миксинов, поэтому биндинги 2.2091 не компилируются (`C1083`), а после их
добавления вылезают расхождения с 2.2081: `CCMouseDelegate::scrollWheel` теперь возвращает
`bool`, `DS_Dictionary::loadRootSubDictFromCompressedFile`/`saveRootSubDictToCompressedFile`
получили второй параметр `bool`, `CCTouchDispatcher::registerForcePrio` — третий `int`, а
`bindings/2.2091/inline/GameObject.cpp` присваивает `m_bDisableScheduling`, которого ни в
заголовках 5.11.2, ни в данных биндингов нет. То есть данные 2.2091 обогнали лоадер.

**Где взять описания 2.2091 — ветка `v6`.** У Geode есть ветка `v6`, это и есть работа по
2.2091:
- её `CMakeLists.txt` в блоке `# begin GEODE_GD_VERSION` объявляет win/mac/ios/android = **2.2091**;
- там лежат `loader/include/Geode/cocos/robtop/touch_dispatcher/CCTouchClipper.h` и
  `loader/include/Geode/cocos/robtop/keyboard_dispatcher/CCKeyboardActionManager.h`
  (PR #2233 «add touch clipper», #2235 «move cctouchclipper and add keyboard action manager»);
- в `CCNode` поле `m_bUnkBool2` переименовано в `m_bDisableScheduling` (PR #2129).

**Важно про раскладку.** `m_bDisableScheduling` — это *переименование* `m_bUnkBool2`, а не новое
поле: количество и порядок полей `CCNode` в 2.2081 и 2.2091 совпадают, значит размеры базовых
классов те же. Массовая проверка (`lab-tools/layout_diff_all.py`, сверка списков полей классов
между `bindings/2.2081` и `bindings/2.2091`): у `PlayerObject` (293 поля), `GameObject` (216),
`GameManager` (164) списки полей совпадают полностью — различия только в написании типов
(`gd::string` ↔ `std::string`, т.е. одно и то же). У 294 cocos2d-классов — тоже совпадают.
Изменённые классы (`CCTextInputNode`, `FLAlertLayer`, `GJGameLevel`, `ShareLevelLayer`, …) нас
не касаются. Отсюда вывод: адреса функций в 2.2091 другие, а **раскладка полей — та же**.

**Ключевой момент: чей SDK брать.** Первая попытка — взять ветку v6 *целиком* как SDK
(`GEODE_SDK=<v6>`, а бинарники релиза подложить в `<v6>/bin/nightly/`, потому что SDK линкует
`${GEODE_BIN_PATH}/nightly/<Geode.lib>`, если каталога с именем версии нет,
`CMakeLists.txt` строка ~377). Биндинги 2.2091 при этом действительно собрались, но
**мод в итоге не загрузился бы**: в v6 прошёл рефакторинг `asp::fs` (PR #2243), и функции,
которые мод импортирует из `Geode.dll`, там имеют другие сигнатуры — например
`utils::file::readJson(asp::fs::PathView)` вместо
`utils::file::readJson(std::filesystem::path const&)`, а `utils::string::pathToString` вообще
исчез. Все они объявлены как `GEODE_DLL`, т.е. импортируются по имени из `Geode.dll`
установленного лоадера (5.11.2) — и таких экспортов там нет.

Верный вариант: **собирать против релизного SDK 5.11.2** (того же, что установлен как лоадер),
а из v6 взять только описания cocos2d-ABI для 2.2091. Они лежат в
`<проект>/sdk-compat/Geode/cocos/...` (427 файлов, 4,7 МБ; 74 из них отличаются от
SDK-овых) и подставляются первыми в include-путь
(`target_include_directories` для `PerGamemodeGlow` и `GeodeBindings`; у CMake каталоги
самого таргета идут раньше, чем каталоги из линкуемых таргетов, поэтому шим побеждает
`/external:I` из SDK). Заготовка делается скриптом:

```powershell
python tools/sync-2.2091-headers.py --sdk "$env:GEODE_SDK" --v6 <путь к клону v6>
```

**Почему копируется всё дерево, а не только изменённые файлы.** Первая версия скрипта
копировала 67 отличающихся файлов — и сборка упала на
`CCTouchClipper.h: ../../cocoa/CCGeometry.h: No such file or directory`. Заголовки cocos2d
включают друг друга *относительными* путями (`../../cocoa/CCGeometry.h`, `"CCNode.h"`), а
такие пути разрешаются от каталога включающего файла, то есть внутри `sdk-compat`, где
остальных файлов нет. Хуже того, частичная подмена неверна по сути: если SDK-овский
`CCSprite.h` дотягивается до `"CCNode.h"` рядом с собой, в одной программе оказываются две
разные версии одного класса (нарушение ODR, а значит неверные `sizeof` и индексы в
vtable). Поэтому копируется дерево целиком: все `Geode/cocos/...` включаются из
2.2091-набора, а не-cocos заголовки (`utils/`, `loader/`, `ui/`) берутся из релизного SDK.
Файлы реализации (`.cpp`/`.mm`) не копируются — они никогда не компилируются в моде.

**Почему нужен именно prepend, а не обычный `target_include_directories`.** SDK добавляет\nсвои четыре каталога (`loader/include`, `.../Geode/cocos/include`, `.../Geode/cocos/extensions`,\n`.../Geode/fmod`) через `target_include_directories(GeodeBindings PUBLIC ...)` внутри\n`add_subdirectory(GEODE_SDK)`. Наш вызов идёт после него, а значит только **дописывается** в\nконец списка: в `build-2091/bindings/GeodeBindings.vcxproj` было\n`...loader/include;...;sdk-compat`, и SDK-овые заголовки 2.2081 выигрывали. Симптом ровно тот\nже, что и без шима: `C2555` (`scrollWheel` возвращает `bool` у `CCTextInputNode`,\n`EditorUI`, `MapSelectLayer`, `ScrollableContentLayer`, `TableView`), `C2065\nm_bDisableScheduling` (`GameObject.cpp`, `SpecialAnimGameObject.cpp`, `SmartGameObject.cpp`),\n`C2660` (`DS_Dictionary::loadRootSubDictFromCompressedFile`/`saveRootSubDictToCompressedFile`\nс двумя аргументами в `GManager.cpp`, `registerForcePrio` с тремя в `CCBlockLayer.cpp`),\n`C2672` в `AccountHelpLayer.cpp` — то есть этот список ошибок и есть точный перечень\nизменений ABI 2.2081 → 2.2091, который закрывают шимы. Лечится перезаписью свойства:\n`INCLUDE_DIRECTORIES` пересобирается как `<наши каталоги> + <то, что было>`. Проверено, что\nв `CMakeLists.txt` SDK нет directory-level `include_directories()`, иначе CMake переставил бы\nих в начало при генерации и prepend не удержался бы.\n\n**Ещё две правки, уже в самом моде, а не в биндингах.** После того как биндинги 2.2091\nсобрались, остались две ошибки в `PerGamemodeGlow.vcxproj`:\n\n1. `Geode/ui/ScrollLayer.hpp:53` и `Geode/ui/Scrollbar.hpp:19` (классы самого лоадера)\n   объявляют `void scrollWheel(float, float) override`, а в 2.2091 у\n   `CCMouseDelegate::scrollWheel` возвращаемый тип `bool` — MSVC ругается C2555.\n   Моду классы UI лоадера не нужны вообще, но они попадали в каждую единицу\n   трансляции, потому что SDK добавляет `<Geode/Geode.hpp>` в precompiled header\n   мода (`target_precompile_headers(${PROJECT_NAME} INTERFACE <Geode/Geode.hpp>)`),\n   а `Geode.hpp` — это `Bindings.hpp + Loader.hpp + UI.hpp + Utils.hpp +\n   modify/Modify.hpp`. Решение: `set(GEODE_DISABLE_PRECOMPILED_HEADERS ON)` до\n   `add_subdirectory(SDK)` (SDK применяет свой дефолт только под\n   `if (NOT DEFINED ...)`, поэтому внешнее значение выигрывает) и в `src/main.cpp`\n   вместо `Geode/Geode.hpp` включены ровно те же «зонтики» без `UI.hpp`:\n   `Bindings.hpp`, `Loader.hpp`, `Utils.hpp`, `modify/Modify.hpp`,\n   `modify/PlayerObject.hpp`. Для GeodeBindings `/FI Geode/Bindings.hpp` остаётся\n   нужным (см. ниже) — он как раз и заменяет исчезнувший PCH.\n2. `sdk-compat/Geode/cocos/support/zip_support/ZipUtils.h` в v6 **смешивает**\n   настоящую правку 2.2091 и свой рефакторинг: нужные изменения —\n   `int unk` в `ccDeflateMemory` и `compressString` и `gd::string` → `std::string`,\n   а лишнее — `unzipAllTo` с типом `asp::fs::Path` и включаемый\n   `<asp/fs/Path.hpp>`. `asp::fs` — API v6, которого в лоадере 5.11.2 нет, поэтому\n   в скрипте синхронизации есть таблица `LOCAL_FIXES`, возвращающая эти две\n   строки к `std::filesystem`. Если v6 изменится и замена не найдётся, скрипт\n   падает с явной ошибкой, а не собирает молча сломанное дерево.\n\nПроверено, что в cocos-заголовках v6 **нет** использования v6-специфичных API (`asp::`,
`arc::`, `GeodeResult`, `nontype_functional` — 0 совпадений), а из внешних заголовков они
включают только `loader/Event.hpp`, `platform/cplatform.h`, `utils/casts.hpp`,
`utils/function.hpp`, `utils/ZStringView.hpp` — всё это в 5.11.2 есть и не менялось. Поэтому
смешивать их с заголовками лоадера 5.11.2 корректно: cocos2d-функции импортируются из
`libcocos2d.dll` игры (то есть нужна именно 2.2091-сигнатура), а Geode-API остаётся
релизным.

В `CMakeLists.txt` теперь две проверки: `sdk-compat/` на месте, и SDK **не** v6-подобный
(если в `loader/include/Geode/utils/file.hpp` встречается `asp::fs` — сборка падает с
объяснением, потому что такой мод не загрузится на релизном лоадере).

```powershell
$env:GEODE_SDK = "<релизный SDK 5.11.2>"
cmake -B build-2091 -G "Visual Studio 18 2026" -A x64
cmake --build build-2091 --config RelWithDebInfo --parallel 6
```

Конфигурация подтверждает: `Mod zam.per-gamemode-glow is compiling for Geometry Dash version
2.2091`.

**Почему мод с целевой версией 2.2091 вообще загрузится релизным лоадером.**
`ModMetadata::checkGameVersion()` в режиме forward-compat **отклоняет** моды с целевой версией
`<= GEODE_GD_VERSION` (2.2081) и **пропускает** те, что новее. Лоадер при этом отключает свои
версионно-зависимые хуки («disable unnecessary hooks in forward compat»), поэтому сам он на
2.2091 стабилен. Это и есть штатный механизм «игра обновилась, Geode ещё нет»: мод собирают под
новую игру на данных новой версии, а лоадер остаётся старым.

**Пятая правка (и последняя на этом пути) — импорт-библиотека cocos2d.** После заголовков и
выключенного PCH компиляция прошла **целиком**, а упала уже линковка: `LNK2019`/`LNK2001`,
ровно 6 неразрешённых символов, и все шесть — из того же перечня изменений ABI 2.2081 → 2.2091:

```
cocos2d::CCNode::applyScale(float)                                  (новый виртуальный)
cocos2d::CCNode::offsetPosition(float, float)                       (новый виртуальный)
cocos2d::CCKeyboardActionManager::handleGlobalAction(int, bool)
cocos2d::CCTouchDispatcher::registerForcePrio(CCObject*, int, int)  (+1 int)
DS_Dictionary::loadRootSubDictFromCompressedFile(char const*, bool) (+1 bool)
DS_Dictionary::saveRootSubDictToCompressedFile(char const*, bool)   (+1 bool)
```

Причина не в заголовках: ссылки на них генерирует сам код биндингов (`GeodeBindings.lib`), а
резолвиться они должны из `loader/include/link/win64/libcocos2d.lib`, который в SDK собран из
**DLL версии 2.2081**. Проверено `dumpbin /exports` по установленной
`lab\Geometry Dash\libcocos2d.dll`: все шесть символов **экспортируются** — то есть игра их
содержит, устарела только импорт-библиотека (в датасете биндингов у них стоит `"win": "link"`,
и `CodegenData.json` этого билда — действительно набор `bindings/2.2091`, там
`loadRootSubDictFromCompressedFile` уже с двумя аргументами).

Лечится генерацией импорт-библиотеки из фактической DLL: `tools/make-2091-cocos-importlib.py`
вызывает `dumpbin /exports`, пишет `.def`, собирает `.lib` через `lib.exe`
(`sdk-compat/libcocos2d-2091.lib`, 5917 экспортов) и проверяет, что среди экспортов есть все
шесть «маркерных» 2.2091-символов (иначе скрипт отказывается работать — это защита от генерации
библиотеки из DLL другой версии). `CMakeLists.txt` падает с подсказкой, если `.lib` нет, и
линкует **обе** библиотеки: неизменившиеся символы одинаковы и резолвятся из любой, изменённые
есть только в нашей. Импорт идёт **по имени**, `delayimp.lib` уже в списке линковки, поэтому
рантайм привяжется к установленной `libcocos2d.dll` при загрузке.

Две ловушки по пути:

1. В `.def` обязательна строка `LIBRARY libcocos2d.dll`. Без неё `lib.exe` назвал бы
   библиотеку по имени `.def`-файла (`libcocos2d-2091.dll`), импорт-дескрипторы ссылались бы
   на несуществующую DLL, и мод не загрузился бы (ошибка была бы уже в игре, а не на сборке).
   Скрипт теперь сам себя проверяет: ищет имя DLL в собранном `.lib`.
2. `target_link_libraries` для этого таргета нужно вызывать **без** ключевого слова
   (`PRIVATE`): `cmake/GeodeFile.cmake:53` использует plain-форму, а CMake запрещает смешивать
   оба вида для одного таргета (`CMake Error ... All uses of target_link_libraries with a
   target must be either all-keyword or all-plain`).

## 8. Проверка в живой игре и горячие клавиши

**Первый запуск (билд без хоткеев).** Лог лоадера подтвердил адреса хуков:
`Enabled PlayerObject::update hook at 0x7ff635b36fc0` (= база `0x7ff635650000` + `0x4e6fc0`)
и `updatePlayerArt hook at 0x7ff635b46bb0` (= база + `0x4f6bb0`), плюс
`loaded 8 mode profile(s)`. Значит пересчёт адресов от базы игры верен, а forward-compat не
мешает `$modify` на `PlayerObject`.

**Что видно на экране** (проверял пользователь, я сверял по кропам скриншотов):

| режим | в конфиге | на экране | вывод |
|-------|-----------|-----------|-------|
| cube | `#FF0000` additive | красный | ✓ |
| ship | `#FF8000` additive | оранжевый (по малиновому фону читается розоватым) | ✓ |
| wave | `#00FFFF` additive | голубой/циан | ✓ |
| robot | `#0000FF` normal | зеленоватый | ✗ — разбираться (см. ниже) |

Четыре разных цвета на четырёх режимах — значит **протечки настроек между режимами нет** и цвет
берётся из конфига. Аддитивное смешивание объясняет «неправильный» оттенок: красное по
фиолетовому фону даёт розовый, по зелёной иконке — жёлто-зелёный; это не баг, а свойство
аддитивного режима (в README про это стоит помнить при выборе `blending`).

**Почему хоткеи сделаны хуком на клавиатуру, а не через Geode-биндинги настроек.** Пользователь
явно отложил GUI: «давай пока json+горячие клавиши, после того как geode перенесут на новую
версию, то gui сделаем» — и в forward-compat экран настроек Geode всё равно недоступен. Поэтому
конфиг остаётся единственным источником истины, а хоткеи только *временно* подстраивают значения
и пишут в лог (в файл ничего не пишется).

**Хук.** `$modify(PGGKeyboardDispatcher, cocos2d::CCKeyboardDispatcher)` с перехватом
`dispatchKeyboardMSG(enumKeyCodes, bool isKeyDown, bool isKeyRepeat, double)`. Это штатный путь:
сам лоадер делает ровно такой же `$modify(CCKeyboardDispatcher)` в `AddExtraKeys.cpp`
(там же видно, что класс не виртуальный — биндинги применяют его через
`GEODE_APPLY_MODIFY_FOR_FUNCTION(..., Thiscall, ..., dispatchKeyboardMSG, ...)`).
Хук **только читает** нажатие и всегда вызывает оригинал с его же результатом, поэтому ввод в
игре (в том числе текстовые поля и бинды) не ломается; реакция только на `isKeyDown && !isKeyRepeat`.
Осторожность: в forward-compat лог печатает `[Forward Compat] CCKeyboardDispatcher new keys` —
лоадер свои версионно-зависимые хуки на этом классе отключает, но наш `$modify` независим
и на 2.2091 работает (проверено в игре).

**Живое перечитывание файла.** `fs::last_write_time` сравнивается со временем, запомненным при
последнем чтении; проверка не чаще раза в 500 мс (счётчик — `std::chrono::steady_clock`, чтобы
не зависеть от `dt`). Файл изменился → `loadConfig()` целиком: профили, пресеты и временные
подстройки сбрасываются к состоянию файла («файл — источник истины»).

**Горячие клавиши.** JSON → `enumKeyCodes`: `F1..F24`, `A..Z`, `0..9`, имена (`space`, `escape`,
`shift`, `left`, …) и сырой код (`0x42`/`66`). Реализованы: `reload`, `preset-next/prev`,
`hue-up/down` (±15° через HSV), `brightness-up/down` (±0.1, множитель, ограничение 0..2).
Подстройки действуют на «текущий режим» — последний, в котором был игрок (`m_lastMode`
обновляется в `apply()`).

**Пресеты.** Именованные наборы *только цветов* по режимам; `brightness`/`blending`/`pulse`
всегда из профиля режима. `"preset": "Ice"` включает пресет при загрузке, смена хоткеем в файл
не пишется. Отсутствующий в пресете режим берёт цвет из своего профиля, поэтому пресеты вида
«все режимы одним цветом» пишутся в одну строку.

**Ловушка сборки (fmt).** Форматирование в `log::*` проверяется **на этапе компиляции**
(`fmt::fstring`), поэтому литеральные `{`/`}` внутри сообщения нужно экранировать как `{{`/`}}`.
Сообщение с примером JSON («`{ \"name\": ... }`») валило сборку `C7595` в
`fmt::report_error`, пока скобки не были удвоены.

**Режим отладки.** `"debug": true` → раз в секунду на режим строка вида
`[debug] ship color=#FF8000 alpha=255 blend=additive preset=none hue=+0 brightness*1.00 | customGlow was off | iconGlow was #4CFF00 a=255 vis=1 | vehicleGlow was … | robot: …`.
Поля `… was` — значения спрайтов **до** нашей записи, то есть то, что в них положила игра
(у `apply()` наоборот логируется после записи, а следующий кадр покажет, перезаписала ли игра
наше значение). Для `robot`/`spider` поле `robot:` дополнительно выводит `ownColor` (цвет1/цвет2
иконки), оба спрайта свечения, тело, конечности и дочерние узлы — именно этот дамп и закрыл
вопрос из раздела 9.

---

## 9. Robot / Spider: цвет из конфига туда не доходит (ограничение игры)

**Итог.** У `robot` и `spider` видимый контур свечения рисует **сама GD** своим свечным проходом,
и берёт цвет **из цвета свечения в наборе иконок** (глобального). Персональный цвет свечения
(`PlayerObject::m_glowColor` + `m_hasCustomGlowColor` — то, что выставляет мод, и то же, что
делает inlined `enableCustomGlowColor()`) для этих двух режимов игра игнорирует. Поэтому
`robot`/`spider` в конфиге не могут повлиять на контур — при любом цвете.

**Как это проверено.** Дампом в debug-логе (поля `ownColor`, `glow`, `extra`, `base`, `head`,
`foot`, `second*`, `part*`, `child*`):

- `m_iconGlow`/`m_vehicleGlow` для этих режимов игра скрывает:
  `iconGlow was #0040FF a=76 vis=0 | vehicleGlow was #0040FF a=76 vis=0`.
- Мы пишем цвет в `m_glowColor`+`m_hasCustomGlowColor` и красим `GJRobotSprite::m_glowSprite` →
  следующий кадр дампа показывает `glow=-:#FF00FF a=255 vis=1`: **наш цвет лежит и спрайт
  «видим»**, но на экране контур остаётся игрового цвета. Значит для robot/spider GD этот спрайт
  не рисует.
- Все реально видимые спрайты (`base`, `head`, `foot`, `part*`, `second*`, `child*`) несут
  **цвета самой иконки** (`ownColor=<цвет1>/<цвет2>`), а не цвет свечения. Белого контура, который
  видит игрок, в дереве спрайтов нет вообще → его рисует не узел сцены, а игровой проход.
- `m_extraSprite` у паука включается (`extra=1 vis=1`), и его тинт попал **в арт**: у паука
  поменялся зрачок внутри глаза, а не контур. Сначала тинт убрали (правило мода: арт иконки не
  перекрашиваем), но потом **вернули по просьбе пользователя** и только для паука — иначе у
  `spider` вообще нет видимого эффекта. У робота та же `m_extraSprite` скрыта (`m_hasExtra = 0`)
  либо несёт деталь арта, поэтому ему тинт не делается.

**Что осталось в коде.** `applyPartGlow()` всё равно красит `m_glowSprite` (ничего не ломает и
заработало бы на билде, где robot/spider читают персональное свечение) + предупреждение в лог при
загрузке, если профиль `robot`/`spider` включён. В README это описано как ограничение игры.

**Почему у остальных 6 режимов всё работает.** Там свечение — это `m_iconGlow`/`m_vehicleGlow`,
которые игра действительно рисует, а мод красит их напрямую; проверено на экране (красный куб,
оранжевый корабль, жёлтый шар, голубая волна и т.д.).

**Ловушка ABI (крэш, который стоил одного прогона).** `CCSprite::displayFrame()` +
`CCSpriteFrame::getFrameName()` в заголовках `sdk-compat` диспатчатся не так, как в
`libcocos2d.dll` игры: вызов вернул мусорный `std::string`, и игра упала с
`std::length_error("string too long")` (стек — `basic_string` ctor внутри нашего dll).
Вывод: **имена кадров спрайтов в рантайме не читать**. Читать безопасно только поля GD-классов
(`CCSpritePart::m_spriteFrameName` — обычное поле) и нетребовательные геттеры
(`getColor`/`getOpacity`/`isVisible`/`getTag`). Весь дамп обёрнут в `try/catch`, чтобы отладочный
вывод не мог уронить игру.

---

## 10. RGB-режим: что выяснилось при настройке «на глаз»

Задача: перелив по всем цветам для всех режимов, включается клавишей. Сделано блоком `rgb` в
конфиге (`speed`/`saturation`/`brightness`/`blending`) плюс три хоткея (`rgb-toggle` `F12`,
`rgb-slower` `PageUp`, `rgb-faster` `PageDown`).

**1. Двойное срабатывание хоткеев (наш баг).** Первое живое нажатие `F12` давало в логе ДВА
события подряд (`RGB mode off` и сразу `RGB mode on`) — переключатель сам себя отменял, со
стороны это выглядело как «клавиша не работает». Причина: один физический key-down может
приходить в `CCKeyboardDispatcher::dispatchKeyboardMSG` больше одного раза (Geode и игра
маршрутизируют событие своими путями). Лечение: `Manager::onKeyEvent` держит флаг «нажата» на
каждый хоткей и игнорирует повторный key-down в пределах 600 мс, а key-up флаг снимает. Таймаут
на случай, если key-up не придёт (потеря фокуса) — иначе хоткей умер бы до конца сессии.
Проверено: одно нажатие = одно действие, два быстрых тапа = два действия, OS-повтор (`repeat=1`)
нажатием не считается.

**2. Аддитивное смешивание «съедало» колесо.** С `"blending": "additive"` перелив выглядел как
два цвета (бирюзовый ↔ фиолетовый). Кадры окна игры (кроп со спрайтом игрока) это подтвердили:
обводка шла фиолетовый → кремовый → синий. Дело не в моде и не в фазе: аддитивное смешивание
**складывает** цвет с фоном уровня, а фон в том уровне насыщенно-синий ⇒ красный + синий даёт
фиолетовый, зелёный + синий — бирюзово-белый, и весь круг сжимается в синюю гамму. Лечение: для
`rgb` по умолчанию `"normal"` — цвет ложится поверх и виден весь круг. Вывод общий: **аддитивный
блендинг нельзя оценивать по одному уровню**, цвет свечения на нём зависит от фона (это же
объясняет раннюю путаницу «красный куб на малиновом фоне читается розовым»).

**3. Фаза перелива сбрасывалась на респавне.** Первая версия считала фазу от
`m_fields->elapsed` — таймера самого `PlayerObject` (того же, что используется для `pulse`).
При перезапуске уровня объект начинает жить заново ⇒ колесо прыгает в красный, что и читалось
как «слишком резко». Лечение: фаза считается по монотонным часам (`monotonicSeconds()`,
`steady_clock` с момента первого вызова) — не зависит ни от таймеров игры, ни от паузы, ни от
числа игроков (в dual оба игрока делят одну фазу). `pulse` по-прежнему использует `elapsed` —
ему как раз нужен таймер игрока.

**4. Цвет менялся мгновенно (щелчок).** Даже непрерывное колесо давало резкий переход при
включении/выключении RGB: из цвета режима (`#F50A0A`) в цвет колеса одним кадром. Добавлен ключ
`fade` (по умолчанию 0.4 с): цвет не ставится сразу, а экспоненциально подтягивается к цели
(`Manager::fadeColor()`), состояние — на каждый режим. В логе видно путь: `#F50A0A` → `#3734C8`
→ `#0A29F5` (lerp между красным и голубым, потом сходится). Побочный эффект полезный: смена
цвета в конфиге и нажатия F8/F9 тоже перестали щёлкать. Два вызова `apply()` за кадр делят
состояние, поэтому второй (с нулевым dt) просто повторяет цвет первого.

**5. Скорость подбирается только на глаз.** Пользователь прошёл 0.25 (4 с/круг — «мигает»),
0.05 (20 с), 0.03 (33 с), 0.02 (50 с — «слишком медленно») и остановился на 0.04 (25 с). Отсюда
две вещи, оформленные как фичи: ступени **круглые** (`kRgbSpeedLadder`, 0.005 … 0.5), а хоткеи
скорости пишут в лог готовую строку `put "speed": 0.04 in the "rgb" block to keep it` — то, что
подобрал на ходу, вписывается в конфиг без угадывания.

**6. Как это выглядит.** С `normal` свечение — цветной ореол вокруг иконки, а не «подсветка
поверх всего»; кому нужен аддитивный вид — `"blending": "additive"` в блоке `rgb`, но тогда
цвет зависит от фона уровня (пункт 2).



