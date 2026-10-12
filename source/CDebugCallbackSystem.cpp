#include "stdafx.h"
#include "CDebugCallbackSystem.h"
#include "CCustomOpcodeSystem.h"
#include "CDebugBridge.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>

namespace CLEO
{
    // Every callback id keeps an ordered list of subscribers. Several plugins
    // may observe the same event, and each subscription records the module it
    // came from so unloading a plugin can drop all of its callbacks before its
    // code is unmapped.
    struct CallbackSubscriber
    {
        uintptr_t fn;
        HMODULE owner;
    };

    static const int CALLBACK_ID_COUNT = CLEO_CB_SCRIPT_OPCODE_REGISTERED + 1;
    static std::mutex g_callbackMutex;
    static std::vector<CallbackSubscriber> g_callbacks[CALLBACK_ID_COUNT];

    // Lock-free mirror of "is anyone observing opcodes". Maintained on the
    // (rare) registration paths so the per-opcode check never takes the mutex.
    static std::atomic<LONG> g_opcodeSubscriberCount{0};

    static void RefreshOpcodeSubscriberCount()
    {
        const LONG count =
            static_cast<LONG>(g_callbacks[CLEO_CB_SCRIPT_OPCODE_PROCESS_BEFORE].size() +
                              g_callbacks[CLEO_CB_SCRIPT_OPCODE_PROCESS_AFTER].size());
        g_opcodeSubscriberCount.store(count);
    }

    static HMODULE CallbackOwner(uintptr_t fn)
    {
        if (fn == 0)
            return nullptr;

        HMODULE module = nullptr;
        if (GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                    GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCSTR>(fn),
                &module))
        {
            return module;
        }

        return nullptr; // core address or unresolvable: keep the subscription
    }

    // Snapshot the subscribers of one id. Callers dispatch from the copy so a
    // callback that registers or unregisters during dispatch cannot corrupt
    // the list we are iterating.
    static void CollectCallbacks(eCallbackId id, std::vector<uintptr_t>& out)
    {
        out.clear();
        if (id < 0 || id >= CALLBACK_ID_COUNT)
            return;

        std::lock_guard<std::mutex> lock(g_callbackMutex);
        for (const auto& subscriber : g_callbacks[id])
            out.push_back(subscriber.fn);
    }

    bool ScriptOpcodeCallbacksActive()
    {
        return g_opcodeSubscriberCount.load() > 0;
    }

    BOOL RegisterCallback(eCallbackId id, uintptr_t callback)
    {
        if (id < 0 || id >= CALLBACK_ID_COUNT || callback == 0)
            return FALSE;

        std::lock_guard<std::mutex> lock(g_callbackMutex);

        for (const auto& subscriber : g_callbacks[id])
        {
            if (subscriber.fn == callback)
                return FALSE; // already subscribed
        }

        g_callbacks[id].push_back(CallbackSubscriber{callback, CallbackOwner(callback)});
        RefreshOpcodeSubscriberCount();
        RefreshOpcodeObservation();
        return TRUE;
    }

    BOOL UnregisterCallback(eCallbackId id, uintptr_t callback)
    {
        if (id < 0 || id >= CALLBACK_ID_COUNT || callback == 0)
            return FALSE;

        std::lock_guard<std::mutex> lock(g_callbackMutex);

        auto& subscribers = g_callbacks[id];
        const size_t before = subscribers.size();
        subscribers.erase(
            std::remove_if(subscribers.begin(), subscribers.end(),
                [callback](const CallbackSubscriber& s) { return s.fn == callback; }),
            subscribers.end());

        if (subscribers.size() != before)
        {
            RefreshOpcodeSubscriberCount();
            RefreshOpcodeObservation();
            return TRUE;
        }
        return FALSE;
    }

    void ReleaseCallbacksForModule(HMODULE module)
    {
        std::lock_guard<std::mutex> lock(g_callbackMutex);

        for (int id = 0; id < CALLBACK_ID_COUNT; ++id)
        {
            auto& subscribers = g_callbacks[id];
            subscribers.erase(
                std::remove_if(subscribers.begin(), subscribers.end(),
                    [module](const CallbackSubscriber& s) { return s.owner == module; }),
                subscribers.end());
        }
        RefreshOpcodeSubscriberCount();
        RefreshOpcodeObservation();
        // Erasing from an already-empty list is a no-op, so repeated calls for
        // the same module are safe.
    }

    void NotifyGameBegin()
    {
        std::vector<uintptr_t> subscribers;
        CollectCallbacks(CLEO_CB_GAME_BEGIN, subscribers);
        for (auto callback : subscribers)
            reinterpret_cast<GameCallback>(callback)();
    }

    void NotifyGameEnd()
    {
        std::vector<uintptr_t> subscribers;
        CollectCallbacks(CLEO_CB_GAME_END, subscribers);
        for (auto callback : subscribers)
            reinterpret_cast<GameCallback>(callback)();
    }

    void NotifyGameProcessBefore()
    {
        std::vector<uintptr_t> subscribers;
        CollectCallbacks(CLEO_CB_GAME_PROCESS_BEFORE, subscribers);
        for (auto callback : subscribers)
            reinterpret_cast<GameCallback>(callback)();
    }

    void NotifyGameProcessAfter()
    {
        std::vector<uintptr_t> subscribers;
        CollectCallbacks(CLEO_CB_GAME_PROCESS_AFTER, subscribers);
        for (auto callback : subscribers)
            reinterpret_cast<GameCallback>(callback)();
    }

    bool NotifyScriptProcessBefore(CRunningScript* script)
    {
        std::vector<uintptr_t> subscribers;
        CollectCallbacks(CLEO_CB_SCRIPT_PROCESS_BEFORE, subscribers);

        // A subscriber may veto processing by returning FALSE; the first veto
        // wins. Later subscribers are still notified so all observers agree.
        bool allow = true;
        for (auto callback : subscribers)
        {
            if (reinterpret_cast<ScriptProcessBeforeCallback>(callback)(script) == FALSE)
                allow = false;
        }

        return allow;
    }

    void NotifyScriptProcessAfter(CRunningScript* script)
    {
        std::vector<uintptr_t> subscribers;
        CollectCallbacks(CLEO_CB_SCRIPT_PROCESS_AFTER, subscribers);
        for (auto callback : subscribers)
            reinterpret_cast<ScriptProcessAfterCallback>(callback)(script);
    }

    int NotifyScriptOpcodeProcessBefore(CRunningScript* script, DWORD opcode)
    {
        if (g_crashSnapshotEnabled != 0 && script != nullptr)
        {
            DWORD offset = 0;
            if (script->GetBytePointer() != nullptr && script->GetBasePointer() != nullptr &&
                script->GetBytePointer() >= script->GetBasePointer() + 2)
            {
                offset = static_cast<DWORD>(script->GetBytePointer() - script->GetBasePointer() - 2);
            }

            CLEO_DebugRecordCrashOpcode(
                reinterpret_cast<uintptr_t>(script),
                script->GetName(),
                opcode,
                offset,
                -1
            );
        }

        std::vector<uintptr_t> subscribers;
        CollectCallbacks(CLEO_CB_SCRIPT_OPCODE_PROCESS_BEFORE, subscribers);

        // All subscribers see the opcode. A HANDLED/INTERRUPT verdict from any
        // one of them wins; PASSTHROUGH leaves dispatch to the next handler.
        int action = CLEO_DEBUG_OPCODE_PASSTHROUGH;
        for (auto callback : subscribers)
        {
            const int result = reinterpret_cast<ScriptOpcodeProcessBeforeCallback>(callback)(script, opcode);
            if (result != CLEO_DEBUG_OPCODE_PASSTHROUGH)
                action = result;
        }

        return action;
    }

    OpcodeResult NotifyScriptOpcodeProcessAfter(CRunningScript* script, DWORD opcode, OpcodeResult result)
    {
        if (g_crashSnapshotEnabled != 0 && script != nullptr)
        {
            DWORD offset = 0;
            if (script->GetBytePointer() != nullptr && script->GetBasePointer() != nullptr &&
                script->GetBytePointer() >= script->GetBasePointer() + 2)
            {
                offset = static_cast<DWORD>(script->GetBytePointer() - script->GetBasePointer() - 2);
            }

            CLEO_DebugRecordCrashOpcodeResult(static_cast<LONG>(result));
        }

        std::vector<uintptr_t> subscribers;
        CollectCallbacks(CLEO_CB_SCRIPT_OPCODE_PROCESS_AFTER, subscribers);

        for (auto callback : subscribers)
        {
            const int callbackResult =
                reinterpret_cast<ScriptOpcodeProcessAfterCallback>(callback)(script, opcode, result);

            if (callbackResult != 0)
                return OR_INTERRUPT;
        }

        return result;
    }

    void NotifyScriptDeleted(CRunningScript* script)
    {
        std::vector<uintptr_t> subscribers;
        CollectCallbacks(CLEO_CB_SCRIPT_DELETED, subscribers);
        for (auto callback : subscribers)
            reinterpret_cast<ScriptDeletedCallback>(callback)(script);
    }

    void NotifyScriptOpcodeRegistered(WORD opcode)
    {
        std::vector<uintptr_t> subscribers;
        CollectCallbacks(CLEO_CB_SCRIPT_OPCODE_REGISTERED, subscribers);
        for (auto callback : subscribers)
            reinterpret_cast<ScriptOpcodeRegisteredCallback>(callback)(opcode);
    }

    void NotifyScriptCreated(CRunningScript* script)
    {
        std::vector<uintptr_t> subscribers;
        CollectCallbacks(CLEO_CB_SCRIPT_CREATED, subscribers);
        for (auto callback : subscribers)
            reinterpret_cast<ScriptCreatedCallback>(callback)(script);
    }
}
