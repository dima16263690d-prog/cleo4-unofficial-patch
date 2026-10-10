# ARCHIVE — Old Runtime / Worker Research (not an active project plan)

> **Status:** historical notes from a separate experimental branch. The Scheduler and worker-execution proposals in this file are abandoned and are not part of the current CLEO 4.4.4 project direction. The active project remains single-threaded on GTA SA's game thread. Do not treat the ideas or roadmap below as tasks to implement. The document is retained only as an archive of past investigation.

---


Внутренний рабочий контекст проекта.

Этот файл предназначен как постоянная техническая память для анализа исходников, результатов экспериментов и архитектурных решений. Это не пользовательская документация и не спецификация готового runtime.

Правило: ничего из этого файла нельзя автоматически считать доказанным только потому, что это здесь записано. Подтвержденные результаты должны сверяться с исходным кодом, GTA SA 1.0 US и реальными тестами.

---

## 0. Точка проекта

Репозиторий:
- dima16263690d-prog/cleo4-unofficial-patch
- ветка: runtime-reverse-architecture-test2
- база ветки: текущий main
- базовый commit на момент создания ветки: 37ea33153eff33a02f4c70a191f6384872a4276c
- цель: CLEO 4 Runtime Reverse Architecture — Test 2

Основная задача Test 2:

Не начинать многопоточность с W0–W5. Сначала полностью разобрать runtime CLEO 4.4.4, его ABI, execution state, opcode dispatch, глобальные зависимости, plugin API, Save/Load и границы GTA main-thread. После этого построить доказанную модель того, какие части можно выполнять параллельно.

Это исследование является продолжением двух линий:
1. рабочий/legacy runtime cleo4-unofficial-patch;
2. экспериментальная многопоточная ветка cleo4nefpath, особенно test-7-two-workers-two-blocks.

---

# 1. Главная архитектурная цель

Многопоточность не считается ошибкой архитектуры сама по себе.

Цель проекта — в будущем получить настоящий параллельный CLEO runtime, в котором несколько независимых execution contexts могут одновременно выполнять безопасную работу, а операции, требующие GTA main thread, проходят через отдельный bridge.

Нельзя делать предположение:

    W0 -> ProcessScript(A)
    W1 -> ProcessScript(B)
    W2 -> ProcessScript(C)

пока не доказано, что ProcessScript и все вызываемые им функции не используют общий mutable state.

Правильная будущая модель:

    CLEO Scheduler
          |
    +-----+-----+-----+-----+-----+-----+
    |     |     |     |     |     |     |
    W0    W1    W2    W3    W4    W5
    |     |     |     |     |     |
    +-----+-----+-----+-----+-----+-----+
                    |
             Execution Context
                    |
             Opcode Classifier
               /       |       \
          CPU_SAFE   BRIDGE    SERIAL
              |         |         |
              v         v         v
           worker    GTA MAIN    lock

Legacy runtime остается fallback:

    Legacy .cs/.cs3/.cs4
             |
             v
    CRunningScript / CCustomScript
             |
             v
    старый ProcessScript

Новый runtime не должен ломать старый.

---

# 2. Жесткие требования совместимости

Целевая игра:
- GTA San Andreas 1.0.0.0 US
- Win32
- Visual Studio 2022
- MSVC v143
- legacy CLEO scripts
- старые .cs/.cs3/.cs4
- старый Sanny Builder
- совместимость с CLEO 4 API/плагинами
- CLEO+ как отдельный plugin layer

Ключевое правило:

Не переписывать CRunningScript только ради многопоточности.

Исследованный legacy ABI для GTA SA 1.0 US:

    CRunningScript size = 0xE0

    0x00 Next
    0x04 Previous
    0x08 Name
    0x10 BaseIP
    0x14 CurrentIP
    0x18 Stack
    0x38 SP
    0x3C LocalVar
    0xBC Timers
    0xC4 IsActive
    0xC5 Cond
    0xC9 ExternalType
    0xCC WakeTime

Связанные адреса:

    AddScriptToQueue = 0x464C00
    active queue     = 0x00A8B42C

Эти данные использовать как исходную ABI-базу и перепроверять при изменениях.

---

# 3. Что уже есть в рабочем CLEO 4 runtime

Основная рабочая ветка cleo4-unofficial-patch содержит:
- HookSystem;
- custom-script lifecycle;
- CCustomScript;
- 0E6F — создание child custom-script из label;
- 0E70 — получение последнего созданного custom-script;
- 0A93 — завершение текущего custom-thread;
- 0AB1/0AB2 — ScmFunction call/return;
- сохранение и восстановление ScmFunction;
- sidecar csN.children.sav;
- sidecar csN.functions.sav;
- восстановление активного scope;
- восстановление string pointers после Load;
- diagnostic logging.

Тестовая цепочка рабочего runtime:

    HookSystem
       |
       v
    0E6F
       |
       v
    CCustomScript parent/child
       |
       v
    0AB1
       |
       v
    ScmFunction
       |
       v
    0AB2
       |
       v
    Save
       +-- csN.sav
       +-- csN.children.sav
       +-- csN.functions.sav
       |
       v
    Load
       |
       v
    restore child + ScmFunction + active scope

Это не надо ломать при исследовании многопоточности.

---

# 4. Самое важное открытие Test 2

Текущий CCustomScript::Process() показывает, почему просто запускать старый CLEO runtime на worker'ах нельзя.

Логика:

    RestoreScriptSpecifics()
            |
            v
    работа с глобальным GTA/CLEO scratch state
            |
            v
    ProcessScript(this)
            |
            v
    StoreScriptSpecifics()

В runtime используются process-global структуры:
- opcodeParams
- missionLocals
- staticThreads
- activeThreadQueue
- inactiveThreadQueue
- GameTimer
- scmBlock
- missionBlock
- MissionLoaded
- script draw/text arrays
- script texture state
- другие GTA engine objects

Поэтому два параллельных вызова старого interpreter могут конфликтовать.

Особенно опасный класс:

    W0: GetScriptParams(A)
    W1: GetScriptParams(B)
    W0: opcodeParams[0] = ...
    W1: opcodeParams[0] = ...

Если opcodeParams общие, один worker может перезаписать результат другого.

---

# 5. Что должно быть исследовано в CLEO 4

Порядок исследования:

    1. CRunningScript
    2. ProcessScript
    3. ScriptExecutionLoop
    4. Opcode dispatch
    5. Parameter system
    6. Stack / locals / condition state
    7. Script queues
    8. CCustomScript state isolation
    9. ScmFunction
    10. Save/Load
    11. plugin API
    12. CLEO+ interaction
    13. GTA pools
    14. RenderWare
    15. global/static variables
    16. thread affinity
    17. opcode classification
    18. only then W0-W5

---

# 6. ScriptExecutionLoop — ключевая точка

Legacy loop делает приблизительно:

    thread
      |
      v
    ReadDataWord()
      |
      v
    opcode
      |
      v
    NotFlag
      |
      v
    opcode handler table
      |
      v
    handler(thread, opcode)

В исследуемом runtime важны:
- opcode fetch;
- instruction pointer;
- operand decoding;
- NOT flag;
- custom opcode routing;
- handler table;
- return value OR_CONTINUE / OR_INTERRUPT;
- exception path;
- diagnostic state.

Потенциальная будущая точка разделения:

    decode
      |
      v
    classify
      |
      +---- CPU_SAFE ------> worker execution
      |
      +---- GTA_MAIN ------> bridge
      |
      +---- SERIALIZED ----> lock/serialized lane
      |
      +---- UNSUPPORTED ---> legacy fallback/error

---

# 7. Параметрическая система

Нужно отдельно изучить:
- GetScriptParams
- SetScriptParams
- TransmitScriptParams
- GetScriptParamPointer
- GetScriptStringParam
- GetScriptParamPointer2
- opcodeParams

В рабочем runtime подтверждена граница 32 parameter slots для 0AB1/0AB2.

Это особенно важно для parallel runtime:

parameter scratch space должен быть привязан к execution context, либо access к legacy global parameter buffer должен быть полностью сериализован.

Нельзя считать API thread-safe только потому, что функция принимает CRunningScript*.

---

# 8. CCustomScript как объект состояния

CCustomScript объединяет:
- legacy CRunningScript;
- code buffer;
- BaseIP;
- CurrentIP;
- CodeSize;
- local variables;
- stack;
- timers;
- condition state;
- custom-script metadata;
- parent/child relationship;
- ScmFunction state;
- textures;
- draw/text snapshots;
- save state;
- compatibility version.

Особенно важно различать:

## Script-local

Можно потенциально сделать private для worker:
- instruction pointer;
- locals;
- stack;
- timers;
- condition state;
- function scope;
- code metadata.

## Global / GTA-owned

Опасно:
- GTA pools;
- RenderWare;
- camera;
- global mission state;
- global queues;
- GTA timers/state;
- global script parameter buffer;
- global draw/text state;
- plugin global state.

---

# 9. 0E6F / Child scripts

Подтвержденная рабочая модель:

    Parent CCustomScript
           |
           +---- Child CCustomScript
                        |
                        +---- same code image/base
                        +---- child label
                        +---- parent relationship
                        +---- separate execution state

Ранее была проблема в старом эксперименте с 0A92, где parentThread не был корректно/полностью инициализирован.

Для Test 2 важно:

child script должен рассматриваться как отдельный execution context, а не просто как второй pointer на тот же mutable runtime state.

---

# 10. ScmFunction

Рабочая модель ScmFunction сохраняет scope:
- previous function ID;
- call IP;
- return address;
- BaseIP;
- CodeSize;
- stack[8];
- SP;
- 32 locals;
- condition result;
- logical operation;
- NOT flag;
- script directory/name;
- string storage.

0AB1:

    caller
      |
      +-- snapshot caller scope
      |
      v
    ScmFunction
      |
      v
    callee scope

0AB2:

    callee
      |
      v
    restore snapshot
      |
      v
    caller

Для многопоточности ScmFunction storage должен стать execution-context-owned.

Нельзя использовать один глобальный mutable store без доказанной синхронизации.

---

# 11. Save/Load

Save/Load — отдельная архитектурная граница.

Legacy:
    csN.sav

должен оставаться совместимым.

Дополнительные состояния:
    csN.children.sav
    csN.functions.sav

Подтвержденная проблема:
- string pointers нельзя просто восстановить как старые адреса;
- при Load string storage может находиться в другом адресе;
- нужны old-pointer/new-storage mappings.

Для будущего parallel runtime:

worker state нельзя сериализовать вместе с OS-thread identity.

При Save надо сохранять execution state, а не сам worker.

То есть:

    Script Context
       |
       +-- workerId = runtime assignment, не persistent identity
       +-- IP
       +-- locals
       +-- stack
       +-- timers
       +-- function scopes
       +-- waiting state
       +-- resources

После Load scheduler может назначить context другому worker.

---

# 12. Test 7 — отдельный исследовательский проект

Репозиторий:
- dima16263690d-prog/cleo4nefpath
- ключевая ветка: test-7-two-workers-two-blocks
- важный research commit: e2ee49a309894ec81cf939993a7fd6c46a30c7d7
- commit message: docs: record CLEO runtime research and six-worker engine roadmap

Test 7 создан не как готовая замена CLEO 4, а как исследовательская платформа.

---

# 13. Что было построено в Test 7

## Smart Memory

Компоненты:

    CCleoMemoryManager
            |
            v
    CSmartMemoryEngine

Идеи:
- CLEO-owned memory;
- отдельная арена;
- Persistent pool;
- Transient pool;
- reuse freed blocks;
- accounting;
- Owns();
- OwnsRange();
- protected release;
- lazy creation/commit;
- диагностика;
- stress testing.

Целевые идеи:
- не резервировать сразу весь 1 GB;
- нормальная целевая емкость около 1 GB;
- hard limit около 2 GB;
- commit по фактической потребности.

Важно:

CLEO memory не должна считаться GTA memory и не должна автоматически перенаправлять GTA allocations.

---

# 14. GTA Memory Engine

Отдельно появился:
    CGtaMemoryEngine

Он намеренно отделен от:
    CCleoMemoryManager
    CSmartMemoryEngine

Он используется как диагностический слой GTA memory, а не как замена GTA allocator.

Принцип:

исследовательский memory engine не должен незаметно менять работу GTA allocator.

---

# 15. Memory Context

Новая идея:

    CCustomScript
          |
          v
    CCleoScriptMemoryContext
          |
          v
    CCleoMemoryManager
          |
          v
    CSmartMemoryEngine

Цель:
- memory ownership привязан к script/context;
- per-script accounting;
- не использовать один общий безымянный memory pool;
- понимать, какой execution context владеет памятью.

Это один из компонентов, который потенциально можно переносить в будущий runtime раньше worker execution.

---

# 16. Worker system Test 7

Основные компоненты:
- CCleoJobSystem
- CWorkerArena
- SPSC rings
- worker-local statistics
- worker-local VM association
- legacy script jobs
- legacy result queues
- execution block scheduling
- job IDs
- worker hints
- pending/peak counters

Worker execution path:

    Scheduler
       |
       v
    CCleoJobSystem
       |
       +--> Worker 0
       +--> Worker 1
       +--> ...

В более поздней архитектурной модели фигурируют W0–W5 как шесть execution lanes.

Но текущий Test 7 branch нельзя описывать как доказанный шести-поточный runtime.

В CCleoBridge.h этой тестовой ветки:
    CLEO_PARALLEL_WORKER_COUNT = 2

То есть текущая reduced configuration — 2 worker lanes.

Комментарии к коду уже закладывают идею шести fixed execution lanes, но это архитектурная цель, а не доказательство работающего полноценного W0–W5 production runtime.

---

# 17. Самое важное ограничение Test 7

В CCustomScript.cpp введен:
    static std::mutex g_legacyScriptProcessMutex;

и:
    constexpr bool CLEO_PARALLEL_LEGACY_SERIALIZE_PROCESS = true;

Смысл:

worker threads существуют, но старый ProcessScript() специально сериализуется.

Следовательно:

    worker infrastructure != true parallel legacy execution

Это нужно помнить всегда.

Если Test 7 показывает:
    W0 active
    W1 active

это не означает, что два legacy opcode interpreter реально исполняются одновременно.

---

# 18. Execution Block

В Test 7 появилась идея execution block/controller.

Приближенная модель:

    Outer / Controller CCustomScript
              |
              v
        Execution Block
              |
              v
          Worker + VM

State зеркалируется между controller и execution block.

Причина:
- сохранить legacy-facing object;
- отделить execution state;
- постепенно вынести runtime из старого object model;
- оставить внешний ABI совместимым.

Это важная архитектурная идея для Test 2.

---

# 19. VM Test 7

Появился CCleoVM.

VM содержит:
- VM context;
- registers;
- instruction pointer;
- locals;
- execution state;
- worker ownership;
- bounded execution steps;
- raw bytecode execution;
- bridge submission.

Есть два режима:
1. собственное экспериментальное VM bytecode;
2. raw/SCM-oriented execution path.

Важно:

Test 7 VM — исследовательский слой, а не доказанный drop-in replacement для всего CLEO 4 opcode runtime.

---

# 20. Worker-local ownership

В Test 7 VM жестко привязан к worker:

    VM N -> Worker N

Идея:
- worker начинает execution;
- выполняет quantum;
- завершает quantum;
- context может продолжить выполнение;
- scheduler не должен одновременно выполнять один context на двух workers.

Это правильное направление для дальнейшего runtime.

---

# 21. WAIT должен освобождать worker

Для будущего scheduler:

    RUNNING
       |
       v
    WAITING
       |
       v
    worker released
       |
       v
    READY
       |
       v
    worker rescheduled

Worker не должен простаивать из-за script WAIT, особенно когда существует много custom scripts.

Это одно из ключевых различий между thread-per-script и job-based execution scheduler.

---

# 22. Bridge

Test 7 построил:
- CCleoBridge
- CGtaCleoBridge

Идея:

    Worker
       |
       v
    SPSC request queue
       |
       v
    GTA MAIN
       |
       v
    GTA operation
       |
       v
    result queue
       |
       v
    Worker

Pointer-free snapshot/command payloads предпочтительны.

Например:
- CleoScriptWorkSnapshot
- CleoBridgeCommand
- CleoBridgeResult

Это лучше, чем передавать живые CCustomScript* через worker boundary.

---

# 23. GTA bridge request model

В Test 7 предусмотрены запросы вроде:
- ReadGameTimer;
- ReadPlayerSnapshot;
- WritePlayerState;
- LegacyOpcodeQuantum;
- RenderWareCommand.

Цель:

worker не должен напрямую модифицировать опасный GTA global state.

---

# 24. RenderWare — отдельная граница

Один из самых важных результатов Test 7:

RenderWare нельзя считать обычной worker-safe частью runtime.

Создан отдельный render request path.

Идея:

    W0 ----\
    W1 -----+--> Render queue --> GTA MAIN --> RenderWare
    W2 ----/

В текущей реализации Render queue и GTA MAIN gate используются как доказательство архитектурного маршрута.

---

# 25. Критическая ошибка RenderWare / shutdown

Исследованный crash:
    0x005D95CE

Стек указывал на область:
    CCustomCarEnvMapPipeline::pluginEnvMatDestructorCB
            |
            v
    __rwPluginRegistryDeInitObject
            |
            v
    _RpMaterialDestroy
            |
            v
    __rpMaterialListDeinitialize

Вывод исследования:

crash локализован до RenderWare material/pipeline teardown, но нельзя утверждать, что единственная причина находится только в одном CLEO class.

Также важно:

уменьшение числа worker threads 4 -> 3 -> 2 само по себе проблему не устранило.

Следовательно:

    worker count != доказанная root cause

Нужно исследовать:
- lifecycle;
- shutdown order;
- RenderWare ownership;
- callback registration;
- resource destruction;
- plugin teardown;
- main-thread affinity.

---

# 26. Почему нельзя объяснять все worker count

Ранее тесты показали, что worker infrastructure могла работать при разных количествах workers.

Но crash мог оставаться.

Следует различать:
- worker scheduling bug;
- race condition;
- resource lifetime bug;
- RenderWare thread-affinity violation;
- shutdown-order bug;
- third-party/plugin interaction;
- stale pointer.

Нельзя использовать число workers как доказательство причины.

---

# 27. Ранее найденные архитектурные ошибки

## 0A92 child/thread experiment

Проблема:
- child создавался;
- parentThread был некорректно/неполностью инициализирован.

Вывод:
- child creation требует полного ownership/context initialization;
- просто аллоцировать CRunningScript недостаточно.

Поэтому для следующего runtime используется более явная модель 0E6F/execution block.

## Save/Load custom state

Проблемы:
- child scripts не помещались корректно в legacy saved-thread model;
- function state нельзя хранить простым hash-only способом;
- pointers на strings становятся недействительными после Load.

Исправление:
- sidecars;
- stable node IDs;
- pointer offsets;
- string old/new mapping;
- отдельное восстановление ScmFunction chain.

## Six-worker interpretation

Ошибочное раннее предположение:

наличие worker infrastructure означает полноценную шестипоточную обработку legacy CLEO.

Фактическая модель Test 7:
- worker infrastructure есть;
- reduced configurations тестировались;
- current branch uses a 2-worker constant;
- legacy ProcessScript сериализован mutex;
- W0–W5 — архитектурная цель, не доказанный production result.

---

# 28. Smart Memory — проблемы и исправления

В Test 7 были проблемы, связанные с:
- lifecycle отдельных memory objects;
- global smart-memory singleton;
- порядок уничтожения static/global объектов;
- повторная инициализация;
- освобождение диапазонов;
- ownership checking;
- различие requested size vs actual allocation.

Исправленная идея:

    CCleoMemoryManager owns CSmartMemoryEngine

а не глобальный:
    gSmartMemory

Это уменьшает риск destruction-order bugs.

---

# 29. Что Test 7 доказал реально

Подтверждено как архитектурные/инфраструктурные результаты:
- отдельный CLEO memory manager возможен;
- отдельная smart-memory arena возможна;
- per-script ownership/context возможен;
- worker threads можно запустить;
- SPSC transport работает как исследовательский механизм;
- worker-local arena возможна;
- VM abstraction возможна;
- controller/execution-block модель возможна;
- GTA bridge нужен;
- RenderWare следует держать за GTA-main boundary;
- legacy ProcessScript нужно защищать/сериализовать, пока зависимости не разобраны.

---

# 30. Что Test 7 НЕ доказал

Не писать в будущем:
- CLEO уже стал полностью многопоточным;
- 6 workers одновременно исполняют legacy .cs;
- ProcessScript thread-safe;
- все opcodes безопасны на workers;
- RenderWare можно безопасно вызывать из worker;
- worker count 6 доказан как оптимальный;
- GTA engine стал многопоточным.

Это все пока не доказано.

---

# 31. Почему Test 2 начинается с Reverse Architecture

Test 2 должен взять лучший результат двух проектов.

От рабочего CLEO 4:
- ABI;
- legacy script compatibility;
- CCustomScript;
- 0E6F;
- 0AB1/0AB2;
- ScmFunction;
- Save/Load;
- plugin compatibility;
- existing hook system.

От NEFPATH/Test 7:
- Smart Memory;
- Memory Context;
- execution block;
- VM abstraction;
- scheduler concept;
- worker system;
- SPSC transport;
- GTA bridge;
- Render queue;
- opcode classification concept;
- diagnostic architecture.

Но не переносить NEFPATH целиком.

---

# 32. Компоненты, которые можно изучать/переносить раньше workers

Приоритет:

    1. Diagnostic context
    2. Smart Memory
    3. Memory Context
    4. Execution Context abstraction
    5. Pointer-free bridge data structures
    6. Opcode classifier
    7. Worker scheduler
    8. One safe worker
    9. Two workers
    10. More workers
    11. six-worker target
    12. whitelist activation

Не начинать с пункта 10.

---

# 33. Proposed Test 2 architecture

    CLEO 4.4.4
         |
      +--+---------------------+
      |                        |
      v                        v
    Legacy Runtime       New Runtime Research
      |                        |
    CRunningScript       Execution Context
    CCustomScript               |
      |                         v
    ProcessScript         Opcode Classifier
      |                    /      |      \
      |               SAFE      BRIDGE   SERIAL
      |                 |          |        |
      |                 v          v        v
      |                W0..W5   GTA MAIN   Lock
      |                             |
      +-----------------------------+
                    |
              Compatibility

Legacy path must remain available throughout the research.

---

# 34. Opcode classification must become data, not guesses

For each opcode, eventually record:

    opcode
    name
    reads_script_state
    writes_script_state
    reads_gta_state
    writes_gta_state
    touches_global_buffer
    touches_renderware
    touches_pool
    touches_filesystem
    touches_audio
    plugin_sensitive
    save_load_sensitive
    thread_safe
    requires_main_thread
    requires_serialization
    fallback_allowed
    notes

Potential classes:
- CPU_SAFE
- CONTEXT_ONLY
- GTA_READ
- GTA_WRITE
- MAIN_THREAD_ONLY
- SERIALIZED
- PLUGIN_UNSAFE
- RENDER_ONLY
- IO
- UNSUPPORTED

---

# 35. Important distinction: CPU parallelism vs GTA parallelism

Future runtime must not try to make GTA San Andreas itself magically multithreaded.

Correct objective:

    Parallelize CLEO computation
            +
    keep GTA engine operations on GTA main thread
            +
    batch/queue bridge operations

So:

    CLEO becomes parallel
    GTA stays main-thread-owned where required

This is the actual architectural target.

---

# 36. Potential future execution quantum

A custom script may run for a bounded quantum:

    READY
      |
      v
    worker
      |
      v
    N instructions/opcodes
      |
      +---- WAIT ----> WAITING
      |
      +---- GTA op --> BRIDGE
      |
      +---- CONTINUE -> READY
      |
      +---- FINISH --> FINISHED
      |
      +---- ERROR ----> ERROR

Do not let a worker execute an unbounded legacy script loop.

Bounded quantum is important for:
- fairness;
- latency;
- avoiding one script monopolizing CPU;
- safe cancellation;
- scheduler control.

---

# 37. Thread state machine

Target model:

    IDLE
     |
     v
    READY
     |
     v
    RUNNING
     |  \
     |   \
     |    +--> WAITING --> READY
     |
     +-------> FINISHED
     |
     +-------> ERROR

Worker ownership:

    RUNNING -> worker assigned
    WAITING -> worker released
    READY -> scheduler chooses worker

A worker is not the persistent identity of a script.

---

# 38. Worker identity vs script identity

Must keep separate:

    script/context ID
    runtime ID
    execution block ID
    job ID
    worker ID
    generation

Do not use:
    workerId == scriptId

and do not persist worker ID in Save/Load as permanent state.

---

# 39. Generation / stale result protection

Future bridge/job systems should use generation counters.

Example:

    script A generation 15
    worker produces result
    script state changes to generation 16
    old result arrives
             |
             v
    discard as stale

This prevents delayed worker/bridge results from corrupting newer execution state.

---

# 40. Main-thread bridge rules

Worker must not directly:
- manipulate RenderWare objects;
- destroy GTA entities;
- modify unsafe GTA pools;
- touch global mission state;
- access global opcode scratch buffers without a designed context;
- invoke unknown plugin callbacks;
- assume a GTA pointer remains valid indefinitely.

Worker should instead submit a description:

    request = {
      type,
      generation,
      parameters,
      contextId
    }

GTA main executes and produces a snapshot/result.

---

# 41. Plugin compatibility is a major obstacle

CLEO plugins may assume:
- current thread == real GTA CRunningScript;
- globals are initialized;
- execution is on main thread;
- TLS/state belongs to one thread;
- current working directory is stable;
- GTA pools are directly accessible;
- callbacks execute synchronously.

Therefore a generic:

    Run plugin opcode on worker

is unsafe by default.

Test 7 introduced thread-local compatibility context ideas:
- thread_local g_parallelWorkerOpcodeExecution
- thread_local g_legacyPluginContextBlock

These are useful research tools, but they do not prove arbitrary third-party plugins thread-safe.

---

# 42. Draw/Text/Texture state is global-sensitive

Current CLEO custom script processing stores/restores:
- script draw arrays;
- script text arrays;
- textures;
- useTextCommands;
- counters;
- shared temporary buffers.

This is another reason that merely moving Process() to worker is insufficient.

For future architecture:

    worker context
       |
       +-- logical draw commands
       |
       v
    main-thread render queue
       |
       v
    GTA Draw/RenderWare

Prefer immutable/render-command data over direct RW pointers.

---

# 43. Research rule for every global

For every global/static used by CLEO, answer:

    Who owns it?
    Who writes it?
    Who reads it?
    Can two workers touch it?
    Can it be copied into context?
    Must it remain main-thread-only?
    Can it be protected by lock?
    Does plugin code assume direct access?

If unanswered:

classify as UNSAFE until proven otherwise.

---

# 44. Test strategy

Every new parallel feature needs three comparisons.

A. Legacy reference
Run the script using normal CLEO 4 path.

B. New runtime
Run through new context/scheduler path.

C. Stress
Run multiple independent contexts and compare:
- final locals;
- IP;
- return values;
- condition state;
- child creation;
- ScmFunction state;
- save/load result;
- GTA side effects;
- error logs.

Expected property:

    New runtime result == Legacy result

for any opcode set declared compatible.

---

# 45. First safe research candidates

Candidate categories are not yet final classifications.

Most promising:
- integer arithmetic;
- float arithmetic;
- local variable manipulation;
- stack operations;
- comparisons;
- logical condition state;
- jumps within private code;
- private string processing;
- execution bookkeeping;
- VM-only bytecode.

Potentially dangerous:
- entity pools;
- world operations;
- camera;
- RenderWare;
- audio;
- global mission state;
- plugin calls;
- file APIs with shared process state;
- direct memory writes into GTA structures.

0AB1/0AB2 are especially interesting because their scope handling is already explicit, but their plugin/opcode environment must still be audited before declaring them worker-safe.

---

# 46. What NOT to do

Never start by:
- removing legacy ProcessScript;
- modifying CRunningScript layout;
- making all opcodes worker-safe by default;
- allowing workers to call RenderWare;
- passing arbitrary GTA pointers between workers;
- replacing Save/Load with worker-specific state;
- deleting compatibility path;
- assuming CLEO+ handlers are thread-safe;
- equating successful worker startup with successful parallel execution.

---

# 47. Desired end state

    +-------------------------+
    |     CLEO Scheduler      |
    +------------+------------+
                 |
       +---------+---------+
       |         |         |
      W0        W1        W2
       |         |         |
      W3        W4        W5
       |         |         |
       +---------+---------+
                 |
          Execution Contexts
                 |
          Opcode Classifier
            /      |      \
           /       |       \
      CPU_SAFE   BRIDGE    SERIAL
         |          |         |
         v          v         v
      worker     GTA MAIN   locked path
                    |
              +-----+-----+
              |           |
              v           v
             GTA      RenderWare

Legacy fallback:

    Unsupported / unsafe opcode
              |
              v
    Legacy ProcessScript path

This allows gradual migration.

---

# 48. Migration sequence

Recommended long-term sequence:

    Phase 0
      Freeze legacy compatibility

    Phase 1
      Reverse-engineer runtime globals and ABI

    Phase 2
      Introduce explicit execution context

    Phase 3
      Move memory ownership into CLEO context

    Phase 4
      Build pointer-free bridge

    Phase 5
      Classify opcodes

    Phase 6
      Execute VM/CPU-safe work on one worker

    Phase 7
      Add WAIT-aware scheduler

    Phase 8
      Add GTA main-thread bridge

    Phase 9
      Run two independent contexts

    Phase 10
      Add more workers

    Phase 11
      Validate W0-W5

    Phase 12
      Enable parallel execution by whitelist

---

# 49. Current Test 2 mission

Immediate research goals:

A. Map the legacy runtime
Find all global/static execution dependencies.

B. Map opcode dependencies
Build a reliable opcode safety table.

C. Define ExecutionContext
Contain everything that can be made private to one script.

D. Define GTA Main boundary
Explicitly enumerate what must be routed to GTA main.

E. Preserve legacy path
No breaking changes to old scripts.

F. Use Test 7 as research reference
Transfer concepts, not wholesale code.

---

# 50. Known research facts to preserve

1. CRunningScript legacy layout is ABI-sensitive.
2. ProcessScript is not proven thread-safe.
3. ScriptExecutionLoop is a key interception/classification point.
4. opcodeParams is a critical global scratch-state candidate.
5. missionLocals is global mission state, not automatically worker-safe.
6. CCustomScript::Process() performs save/restore around legacy global state.
7. 0E6F child scripts need explicit parent/ownership initialization.
8. 0AB1/0AB2 depend on explicit function scope state.
9. Save/Load needs pointer-safe restoration.
10. Smart Memory can be isolated from GTA memory.
11. Worker infrastructure can be built without making legacy execution parallel.
12. Test 7 deliberately serialized ProcessScript with a mutex.
13. RenderWare must be treated as main-thread-sensitive.
14. 0x005D95CE was localized to RenderWare material/pipeline teardown, but root cause is not conclusively isolated.
15. Reducing worker count did not prove or eliminate the shutdown problem.
16. Current Test 7 reduced bridge configuration uses two workers; six workers remain an architectural target, not a proven final runtime.
17. Worker ID must not be treated as persistent script identity.
18. WAIT should release the worker.
19. Plugin compatibility is a separate thread-safety problem.
20. Parallel CLEO does not imply making GTA San Andreas itself multithreaded.

---

# 51. Working terminology

Use these terms consistently.

Legacy Runtime
Old CLEO 4 execution path.

Execution Context
Private mutable state required to continue one script.

Execution Block
Runtime object representing the active executable state for a context/controller.

Controller
Legacy-facing CCustomScript object owning/representing the logical script.

Worker
OS thread executing a bounded quantum.

Scheduler
Selects READY contexts and assigns work.

Bridge
Transport between worker-side execution and GTA main thread.

GTA MAIN
The thread allowed to perform main-thread-sensitive GTA operations.

CPU_SAFE
Operation that can run without touching unsafe shared state.

SERIALIZED
Operation that may run outside main thread but requires controlled serialization.

UNSUPPORTED
Operation not yet safe/ported.

---

# 52. Final architectural principle

The project must not ask:

How do we force CLEO 4 to use six threads?

It must ask:

What exact state and operations does one CLEO execution require, which parts can be owned by one execution context, and which operations must cross the GTA main-thread boundary?

Once that map is complete, six workers become a scheduling problem rather than a blind reverse-engineering gamble.

The legacy engine remains the compatibility anchor.

The new runtime grows around it.

    Legacy compatibility
           +
    Execution Context isolation
           +
    Opcode classification
           +
    GTA Main Bridge
           +
    Scheduler
           +
    W0..W5

That is the intended direction for CLEO 4 Runtime Reverse Architecture — Test 2.
