# CLEO 4.4.4 Unofficial Patch — v1.0 TEST

## Рабочая точка v1.0 TEST

**Baseline:** `151936070c594fe801ffec48b11bf17527b4b00d`

Это зафиксированная тестовая исходная точка проекта. Она предназначена для сборки и повторного тестирования исправлений `HookSystem`, custom-script lifecycle, `0E6F`, `ScmFunction`, `0AB1/0AB2` и Save/Load.

### Что зафиксировано

- `HookSystem` и работа хуков.
- Исправления памяти/lifecycle для `CCustomScript`.
- `0E6F` — создание child custom-script из label.
- `0E70` — получение последнего созданного custom-script.
- `0A93` — завершение текущего custom-thread.
- `0AB1` / `0AB2` — вызов и возврат `ScmFunction`.
- `ScmFunction` lifecycle, восстановление и очистка scope.
- Сохранение child-state в `csN.children.sav`.
- Сохранение состояния `ScmFunction` в `csN.functions.sav`.
- Восстановление активного `ScmFunction`/scope после Save/Load.
- Исправление восстановления string pointers после Load.
- Диагностический лог `cleo_diagnostic.log`.
- Проверенная схема parent → child → Save → Load → restore.

## Save/Load

Используются отдельные sidecar-файлы:

```text
cleo/cleo_saves/csN.sav
cleo/cleo_saves/csN.children.sav
cleo/cleo_saves/csN.functions.sav
```

Legacy `csN.sav` не заменяется новым форматом для этих функций.

`functions.sav` хранит состояние `ScmFunction`; при восстановлении исправлено перепривязывание указателей строковых параметров к новой `std::string` storage.

## Runtime target

- GTA San Andreas 1.0 US
- Win32
- Release / GTASA
- Visual Studio 2022 / MSVC v143

## Тестовая цепочка

```text
HookSystem
   ↓
0E6F
   ↓
CCustomScript parent/child
   ↓
0AB1 → ScmFunction → 0AB2
   ↓
Save
   ├── csN.sav
   ├── csN.children.sav
   └── csN.functions.sav
   ↓
Load
   ↓
restore child + ScmFunction + active scope
```

## Подтверждённые исправления

`151936070` исправляет сборку восстановления `ScmFunction` под MSVC. В `source/CScriptEngine.cpp` восстановление string locals больше не использует `std::map`; применяются пары старых/новых адресов и единый `restoreStringPointer`, который обрабатывает как `savedTls`, так и текущие 32 locals. Это устраняет проблему перепривязки указателей после восстановления. 

Предыдущий этап `d72b4a6` завершил sidecar Save для `ScmFunction`, включая безопасное имя `csN.functions.sav` и бинарные Read/Write helpers.

## Возврат к v1.0 TEST

Эталонный commit:

```text
151936070c594fe801ffec48b11bf17527b4b00d
```

В Git:

```powershell
git fetch origin
git checkout main
git reset --hard 151936070c594fe801ffec48b11bf17527b4b00d
```

Или переключиться на защищённую тестовую ветку:

```powershell
git fetch origin
git checkout v1.0-test
```

## Важно

Это **v1.0 TEST**, а не финальный релиз. Новые эксперименты не должны изменять эту рабочую точку без отдельной ветки/коммита.

Ненужные экспериментальные изменения после этой точки в v1.0 TEST не входят.

## Направление проекта и исследование CLEO 5

Проект развивается на основе собственного исследования **CLEO 4.4.4**, а также изучения исходного кода и архитектурных решений **CLEO 5**.

Исходники CLEO 5 используются как исследовательская база и источник архитектурных идей, **но проект не является копированием CLEO 5**. Цель — улучшить именно CLEO 4.4.4, сохранив его legacy-основу и совместимость со старыми скриптами.

Основная концепция:

```text
CLEO 4.4.4 Legacy
      │
      ├── старые opcode
      ├── .cs / .cs3 / .cs4
      ├── legacy Execution Engine
      └── совместимый ABI
             │
             ▼
     Исследование CLEO 5
     исходники + архитектура
             │
             ▼
       Адаптация идей
             │
             ▼
      Улучшенный CLEO 4
             │
      ┌──────┼───────────┐
      ▼      ▼           ▼
   Plugin  DebugUtils  Memory
     SDK                Manager
                          │
                          ▼
                   Custom Runtime
```

### Что именно улучшается

- сохранение старых CLEO 4 opcode;
- сохранение совместимости `.cs/.cs3/.cs4`;
- сохранение legacy `CRunningScript` и существующей модели выполнения;
- перенос полезных архитектурных решений в более современную модульную систему;
- развитие нового Plugin SDK;
- централизованная диагностика через DebugUtils;
- отдельная CLEO Memory Manager;
- контролируемый GTA/CLEO Bridge;
- подготовка Custom Runtime и Custom VM;
- возможность будущих Worker Contexts без принудительного перевода legacy scripts на новый runtime.

### Главный принцип

> **Не заменить CLEO 4 на CLEO 5, а развить CLEO 4.4.4 с учётом результатов исследования CLEO 5.**

Новые подсистемы должны добавляться рядом со старым ядром и не ломать существующие скрипты и opcode.


## Audio: legacy + extended API

В ветке `test-xx02` Audio развивается по комбинированной схеме.

### Legacy CLEO 4

Старые аудио opcode остаются в основном ядре и сохраняют свою интерфейсную семантику:

`0AAC`, `0AAD`, `0AAE`, `0AAF`, `0AB9`, `0ABB`, `0ABC`, `0AC0`, `0AC1`, `0AC2`, `0AC3`, `0AC4`, `0AC5`.

Для `0AAD` сохранены отдельные legacy-пути `LegacyPlay()` и `LegacyStop()`, чтобы обновление внутреннего audio runtime не меняло старое поведение скриптов.

### Updated BASS

`third-party/bass/` обновлён комплектом `bass.dll`, `bass.lib` и `bass.h` из актуального исследуемого baseline CLEO 5.

При этом CLEO 4 не запускает второй BASS engine: один существующий `CSoundSystem` остаётся владельцем stream handles.

### Audio.cleo

Добавлен отдельный plugin:

`demo_plugins/Audio/Audio/Audio.vcxproj`

Он регистрирует новые аудио opcode:

`2500`–`250C`.

Новые функции работают с теми же stream handles, которые возвращаются старым `0AAC/0AC1`.

Таким образом:

```text
Legacy 0AAC/0AC1
       │
       ▼
 CLEO 4 CAudioStream
       │
       ├── old 0AAD/0AAE/0AAF...
       │
       └── Audio.cleo
              │
              └── 2500–250C
```

Это позволяет постепенно переносить полезные Audio-возможности из исследования CLEO 5 в CLEO 4, не заменяя legacy audio layer.

Интеграционный тест: `tests/AUDIO_25XX_TEST.cs`.


## Текущее состояние разработки — test-xx02

На этом этапе ветка `test-xx02` содержит проверенное развитие проекта поверх базового CLEO 4.4.4. Основной принцип сохраняется: улучшать CLEO 4, не ломая legacy-скрипты, legacy opcode и существующую модель совместимости.

### DebugUtils — самостоятельная система диагностики

Добавлен и расширен отдельный `DebugUtils.cleo`, предназначенный для лёгкой диагностики CLEO/GTA SA без постоянной тяжёлой crash-аналитики в обычном игровом цикле.

Что уже сделано:

- отдельные структурированные логи для core, scripts, memory, diagnostics и crash reports;
- фоновая запись script-log через ограниченную очередь, чтобы диск не обслуживался внутри script callback;
- дедупликация повторяющихся сообщений и ротация core-log на 1 MiB вместо старого жёсткого лимита;
- отдельный crash handler с lazy-подключением DbgHelp только в crash path;
- подробный снимок `EXCEPTION`, `FAULT`, `CPU`, `INSTRUCTION`, `MEMORY`, `PROCESS MEMORY`;
- сохранение CLEO-контекста: script, opcode, script offset, opcode result и game tick;
- встроенная verified CrashInfo база с exact-address/module/RVA/context matching;
- отдельная `CLEO-CrashAuto.txt` для новых наблюдений без автоматического превращения их в verified записи;
- русская локализация CrashInfo и crash window;
- честное разделение контекста и причинности: script/last opcode не объявляются причиной без подтверждения;
- crash-only StackWalk64/legacy/EBP попытки с `STACK_SCAN` fallback;
- `STACK_SCAN` помечается как `heuristic=1`, а faulting EIP всегда сохраняется как frame `#00`;
- добавлен `fault_ip_present=1/0`, чтобы отчёт явно показывал наличие faulting IP в backtrace;
- heuristic backtrace не используется для загрязнения verified CrashInfo matcher/fingerprint.

Подтверждённый тестовый результат для воспроизводимого `0xC0000005`:

```text
status=OK method=STACK_SCAN heuristic=1
frames=25
fault_ip_present=1
#00 address=0x005D95CE module=gta_sa.exe rva=0x001D95CE
```

Это означает, что при отсутствии пригодного обычного unwind DebugUtils всё равно получает полезный crash snapshot и эвристический стек, не выдавая его за гарантированный call chain.

### Audio.cleo — расширенный аудио API

Добавлен отдельный plugin `Audio.cleo`, который расширяет аудио-возможности CLEO 4, не создавая второй audio engine и не дублируя владение BASS stream handles.

Добавлены расширенные opcode `2500`–`250D`:

- `2500` — playing state;
- `2501` — duration;
- `2502` / `2503` — speed read/set;
- `2504` — volume transition;
- `2505` — speed transition;
- `2506` — 3D source size;
- `2507` / `2508` — normalized progress read/set;
- `2509` / `250A` — stream type read/set;
- `250B` / `250C` — progress in seconds read/set;
- `250D` — looping control.

`2500`–`250C` реализуют расширенный CLEO 5-style набор управления уже существующими CLEO 4 stream handles. `250D` — наш собственный дополнительный opcode для управления looping.

Для Sanny Builder добавлены соответствующие записи в `opcodes.txt` и `SASCM.INI`. Добавлен интеграционный тест `tests/AUDIO_25XX_TEST.cs`.

### Audio core / API

Для нового plugin API добавлен отдельный `CAudioPluginAPI`, а существующий `CSoundSystem` расширен так, чтобы новый Audio.cleo работал через общий CLEO 4 audio layer. Обновлён bundled BASS baseline; второй BASS engine в Audio.cleo не создаётся.

## Что ещё осталось сделать DebugUtils

Текущая crash-диагностика уже рабочая, поэтому дальнейшие задачи — улучшения качества, а не обязательное исправление базового crash path:

- добавить optional `SymFromAddr` и вывод имён функций там, где символы реально доступны;
- при необходимости расширить Crash report списком загруженных `.asi` / `.cleo` модулей;
- добавить opt-in `OpcodeHistory` с несколькими последними opcode в crash context;
- сделать diagnosis информативнее, отделяя known CrashInfo signature, stack presence и фактическую причинность;
- улучшить качество heuristic stack scan без переименования его в полноценный unwind;
- расширить crash test matrix на дополнительные воспроизводимые типы аварий и unknown-crash сценарии;
- после накопления тестов зафиксировать финальный lightweight performance contract и завершённый статус DebugUtils.

### Что сознательно не входит в текущий DebugUtils

DebugUtils не пытается автоматически «чинить» GTA, не объявляет CLEO script виновным только по последнему opcode и не выдаёт heuristic stack scan за доказанную цепочку вызовов. Более глубокие game-specific hooks добавляются только под конкретно воспроизведённую проблему.