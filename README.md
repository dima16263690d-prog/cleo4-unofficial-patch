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
