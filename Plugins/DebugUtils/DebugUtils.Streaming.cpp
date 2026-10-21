// Streaming-model attribution for failed texture reads.
//
// A failed RwTextureRead during streaming is usually a .txd that references a
// texture by name which is not present in it (or in a parent .txd), or a model
// whose texture dictionary is missing/corrupt. The texture NAME alone does not
// say which FILE to look at - and SA does not keep a model's name anywhere in
// memory (CBaseModelInfo stores only a hash). This file answers "which file was
// being streamed?" by:
//
//   1. Hooking CStreaming::ConvertBufferToObject (SA 1.0 US 0x40C6B0) with the
//      shared 5-byte JMP detour. Everything the RW loader does for one model
//      (including the texture reads) runs between its entry and exit, so a
//      thread-local "model currently being converted" is exactly the model that
//      owns a texture read failing inside it.
//   2. Building a modelId -> file name map once, by reading the IMG archives'
//      directories (the only place the names exist).
//
// Both steps are best-effort and opt-in. The hook is guarded exactly like the
// texture exact-caller hook (refuses on anything unexpected); the name index
// fails silently (names simply stay unavailable) on any file/format problem.
#include "DebugUtils.h"
#include "DebugUtils.Internal.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace
{
    // SA 1.0 US addresses (verified against gta-reversed and plugin-sdk).
    constexpr DWORD kGtaSa10ConvertBufferToObject = 0x40C6B0;
    // CModelInfo::ms_modelInfoPtrs / ms_modelInfoCount, reached the way the game
    // and plugin-sdk reach them (a pointer embedded in an instruction operand plus
    // a count slot), so a limit adjuster that relocates the array is respected.
    constexpr DWORD kGtaSa10ModelInfoPtrsRef = 0x40CD67;  // -> CBaseModelInfo**
    constexpr DWORD kGtaSa10ModelInfoCount = 0x40CD58;    // int
    constexpr DWORD kGtaSa10StreamingFiles = 0x8E48D8;    // tStreamingFileDesc ms_files[8]
    constexpr DWORD kGtaSa10TxdPool = 0xC8800C;           // CPool<TxdDef>* ms_pTxdPool

    constexpr int kStreamingFileCount = 8;
    constexpr int kMaxModelInfos = 65536;  // sanity bound on a corrupt count
    constexpr int kMaxImgEntries = 200000; // sanity bound on a corrupt header

    // CBaseModelInfo prefix: a vtable pointer, the name hash, refcount and the
    // index of the texture dictionary (TXD) the model uses. The TXD index is how
    // a failed texture is tied to the .txd file that should contain it.
    struct BaseModelInfoHead
    {
        void* vtable;            // 0x00
        unsigned int key;        // 0x04  m_nKey (name hash)
        unsigned short refCount; // 0x08  m_nRefCount
        short txdIndex;          // 0x0A  m_nTxdIndex (slot in CTxdStore, -1 if none)
        unsigned char alpha;     // 0x0C
        unsigned char num2d;     // 0x0D
        short effect2d;          // 0x0E
        short objectInfo;        // 0x10
        unsigned short flags;    // 0x12  m_nFlags (bit 5 = bIsLod)
    };
    constexpr unsigned short kModelFlagIsLod = 1u << 5;

    // tStreamingFileDesc: name at offset 0, flags/handle after. Only the name is
    // read here, but the full prefix is declared so the layout is asserted.
    struct StreamingFileDescHead
    {
        char name[40];             // m_szName, offset 0x00
        unsigned char notPlayerImg; // 0x28
        unsigned char pad;          // 0x29
        unsigned short pad2;        // 0x2A
        unsigned int streamHandle;  // 0x2C
    };
    static_assert(sizeof(StreamingFileDescHead) == 0x30, "StreamingFileDesc layout");

    // TxdDef: the dictionary, refcounts, parent and the name hash.
    struct TxdDefHead
    {
        void* rwDictionary;
        short refsCount;
        short parentIndex;
        unsigned int hash;
    };
    static_assert(sizeof(TxdDefHead) == 0xC, "TxdDef layout");

    // CPool<TxdDef> prefix up to (and including) the size field.
    struct TxdPoolHead
    {
        TxdDefHead* objects;
        unsigned char* byteMap;
        int size;
    };

    // A CPool slot holds a live object when its byte-map "empty" bit (bit 7) is clear.
    bool PoolSlotUsed(const unsigned char* byteMap, int index)
    {
        return (byteMap[index] & 0x80) == 0;
    }

    // CStreaming::ConvertBufferToObject. gta-reversed decompiles it as
    // bool(uint8*, int32); plugin-sdk lists a 3rd ChannelIndex. The wrapper accepts
    // and forwards all three, which is correct either way (cdecl caller-cleanup).
    typedef char(__cdecl* ConvertBufferToObjectFn)(char* fileBuffer, int modelId, int channelIndex);
    ConvertBufferToObjectFn g_convertBufferToObjectOriginal = nullptr;

    // The model being converted on THIS thread, or -1. thread_local because a
    // texture read can happen on a non-game thread (a mod's loader).
    thread_local int g_streamingModelId = -1;

    // Cache: modelId -> model facts (TXD slot + LOD flag), filled on demand from
    // the live model-info array so it tracks the current game.
    struct ModelFacts
    {
        int txdIndex;
        bool isLod;
    };
    std::unordered_map<int, ModelFacts> g_modelFacts;
}

// The TXD slot and LOD flag of a model (a single lookup, cached). The slot names
// the .txd file a failed texture should have come from; the LOD flag lets the
// report say that a reduced LOD texture set is expected.
std::pair<int, bool> DebugUtils::ModelTxdIndexAndLod(int modelId)
{
    if (modelId < 0)
        return {-1, false};

    const auto cached = g_modelFacts.find(modelId);
    if (cached != g_modelFacts.end())
        return {cached->second.txdIndex, cached->second.isLod};

    ModelFacts facts{-1, false};
    auto** infos = *reinterpret_cast<BaseModelInfoHead***>(kGtaSa10ModelInfoPtrsRef);
    const int modelInfoCount = *reinterpret_cast<const int*>(kGtaSa10ModelInfoCount);
    if (infos != nullptr && modelId < modelInfoCount && modelInfoCount <= kMaxModelInfos &&
        infos[modelId] != nullptr)
    {
        facts.txdIndex = infos[modelId]->txdIndex;
        facts.isLod = (infos[modelId]->flags & kModelFlagIsLod) != 0;
    }

    g_modelFacts.emplace(modelId, facts);
    return {facts.txdIndex, facts.isLod};
}

char __cdecl DebugUtils::ConvertBufferToObjectWatch(char* fileBuffer, int modelId, int channelIndex)
{
    ConvertBufferToObjectFn original = g_convertBufferToObjectOriginal;
    if (original == nullptr)
        return 0; // no trampoline: refuse rather than run the body wrongly

    // Save/restore so a nested conversion (there is none today) cannot corrupt the
    // outer one's context.
    const int previous = g_streamingModelId;
    g_streamingModelId = modelId;
    const char result = original(fileBuffer, modelId, channelIndex);
    g_streamingModelId = previous;
    return result;
}

int DebugUtils::CurrentStreamingModelId()
{
    return g_streamingModelId;
}

// The game's CKeyGen::GetUppercaseKey (CRC-32, polynomial 0xEDB88320), computed
// bitwise so no 256-entry table has to be embedded. Used only while building the
// name index (a few tens of thousands of calls), never on a hot path.
unsigned int DebugUtils::KeyHashUpper(const char* str)
{
    unsigned int crc = 0xFFFFFFFFu;
    for (; *str != '\0'; ++str)
    {
        unsigned char c = static_cast<unsigned char>(*str);
        if (c >= 'a' && c <= 'z')
            c = static_cast<unsigned char>(c - 0x20);
        crc ^= c;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return crc;
}

// Read one IMG archive's directory and map every DFF/TXD entry's name to the model
// id the engine assigns it. The engine strips the extension, then either finds a
// model by name hash (DFF) or a TXD slot by name hash (TXD). Anything unexpected
// (missing file, wrong magic, short read) just ends the scan for that archive.
void DebugUtils::LoadImgNames(
    const char* path,
    const std::unordered_map<unsigned int, int>& modelByKey,
    const std::unordered_map<unsigned int, int>& slotByHash)
{
    FILE* fp = fopen(path, "rb");
    if (fp == nullptr)
    {
        // The streaming entry may hold a path relative to the game directory while
        // the process CWD differs; retry against the module's own directory.
        char modulePath[MAX_PATH] = {};
        const DWORD len = GetModuleFileNameA(nullptr, modulePath, MAX_PATH);
        if (len == 0 || len >= MAX_PATH)
            return;
        char* slash = strrchr(modulePath, '\\');
        if (slash == nullptr)
            return;
        *(slash + 1) = '\0';
        std::string full(modulePath);
        full += path;
        fp = fopen(full.c_str(), "rb");
        if (fp == nullptr)
            return;
    }

    char version[4];
    int count = 0;
    if (fread(version, 1, sizeof(version), fp) != sizeof(version) ||
        memcmp(version, "VER2", sizeof(version)) != 0 ||
        fread(&count, sizeof(count), 1, fp) != 1 ||
        count <= 0 || count > kMaxImgEntries)
    {
        fclose(fp);
        return;
    }

    // Directory entry: uint32 pos, uint16 size, uint16 sizeInArchive, char name[24].
    for (int i = 0; i < count; ++i)
    {
        unsigned char entry[32];
        if (fread(entry, 1, sizeof(entry), fp) != sizeof(entry))
            break;

        char name[24];
        memcpy(name, entry + 8, sizeof(name));
        name[sizeof(name) - 1] = '\0';

        char* dot = strchr(name, '.');
        if (dot == nullptr)
            continue;

        const bool isDff = _stricmp(dot + 1, "DFF") == 0;
        const bool isTxd = !isDff && _stricmp(dot + 1, "TXD") == 0;
        if (!isDff && !isTxd)
            continue;

        *dot = '\0'; // the engine hashes the extension-stripped name
        const unsigned int key = KeyHashUpper(name);

        if (isDff)
        {
            const auto it = modelByKey.find(key);
            if (it != modelByKey.end())
                m_modelNames[it->second] = name;
        }
        else
        {
            const auto it = slotByHash.find(key);
            if (it != slotByHash.end())
                m_modelNames[kTxdModelBase + it->second] = name;
        }
    }

    fclose(fp);
}

// Build the modelId -> name map once. The hash->id maps are built first so each
// IMG entry is an O(1) lookup instead of a scan of 20000 model infos.
void DebugUtils::EnsureModelNameIndex()
{
    if (m_modelNameIndexBuilt)
        return;
    m_modelNameIndexBuilt = true; // set first: a failure must not retry every frame

    // The rebuild is also the per-game reset point (OnGameBegin clears
    // m_modelNameIndexBuilt): the model-info array is rebuilt for the new game, so
    // the cached TXD/LOD facts from the previous game must not survive.
    g_modelFacts.clear();

    std::unordered_map<unsigned int, int> modelByKey;
    auto** infos = *reinterpret_cast<BaseModelInfoHead***>(kGtaSa10ModelInfoPtrsRef);
    const int modelInfoCount = *reinterpret_cast<const int*>(kGtaSa10ModelInfoCount);
    if (infos == nullptr || modelInfoCount <= 0 || modelInfoCount > kMaxModelInfos)
        return;
    for (int i = 0; i < modelInfoCount; ++i)
    {
        if (infos[i] != nullptr)
            modelByKey.emplace(infos[i]->key, i);
    }

    std::unordered_map<unsigned int, int> slotByHash;
    auto* pool = *reinterpret_cast<TxdPoolHead**>(kGtaSa10TxdPool);
    if (pool != nullptr && pool->objects != nullptr && pool->byteMap != nullptr &&
        pool->size > 0 && pool->size <= kMaxModelInfos)
    {
        for (int i = 0; i < pool->size; ++i)
        {
            if (PoolSlotUsed(pool->byteMap, i))
                slotByHash.emplace(pool->objects[i].hash, i);
        }
    }

    auto* files = reinterpret_cast<StreamingFileDescHead*>(kGtaSa10StreamingFiles);
    for (int f = 0; f < kStreamingFileCount; ++f)
    {
        if (files[f].name[0] != '\0')
            LoadImgNames(files[f].name, modelByKey, slotByHash);
    }

    WriteFrameTime(
        "[streaming] model-name index built: %zu name(s) from %d archive(s) "
        "(names unavailable for models in archives that could not be read)",
        m_modelNames.size(), kStreamingFileCount
    );
}

std::string DebugUtils::ModelNameForId(int modelId)
{
    if (modelId < 0)
        return {};
    EnsureModelNameIndex();
    const auto it = m_modelNames.find(modelId);
    return it == m_modelNames.end() ? std::string() : it->second;
}

void DebugUtils::InstallStreamingModelWatch()
{
    if (!m_streamingModelNamesEnabled || m_streamingHookInstalled)
        return;

    if (CLEO_GetGameVersion() != GV_US10)
    {
        WriteFrameTime(
            "[streaming] disabled: CStreaming addresses are SA 1.0 US only"
        );
        return;
    }

    DWORD target = m_streamingModelAddress;
    if (target == 0)
        target = kGtaSa10ConvertBufferToObject;

    const DWORD imageBase = reinterpret_cast<DWORD>(GetModuleHandleA(nullptr));
    if (target < imageBase || target >= imageBase + 0x01000000)
    {
        WriteFrameTime(
            "[streaming] StreamingModelAddress=0x%08X is outside the game image "
            "(base 0x%08X); using the built-in default 0x%08X instead",
            static_cast<unsigned>(target),
            static_cast<unsigned>(imageBase),
            static_cast<unsigned>(kGtaSa10ConvertBufferToObject)
        );
        target = kGtaSa10ConvertBufferToObject;
    }

    void* trampoline = nullptr;
    if (!InstallEntryDetour(
            target,
            reinterpret_cast<const void*>(&DebugUtils::ConvertBufferToObjectWatch),
            m_streamingSaved,
            &trampoline,
            "streaming"))
    {
        return;
    }

    m_streamingTarget = reinterpret_cast<void*>(target);
    g_convertBufferToObjectOriginal = reinterpret_cast<ConvertBufferToObjectFn>(trampoline);
    m_streamingHookInstalled = true;

    WriteFrameTime(
        "[streaming] enabled: CStreaming::ConvertBufferToObject at 0x%08X is hooked; "
        "a failed texture read names the model/txd being streamed",
        static_cast<unsigned>(target)
    );
}

void DebugUtils::RemoveStreamingModelWatch()
{
    if (!m_streamingHookInstalled || m_streamingTarget == nullptr)
        return;

    RemoveEntryDetour(
        reinterpret_cast<DWORD>(m_streamingTarget),
        reinterpret_cast<const void*>(&DebugUtils::ConvertBufferToObjectWatch),
        m_streamingSaved,
        "streaming");

    // The trampoline is never freed (a thread may be running inside it) and the
    // global pointer is kept: if our JMP is still reachable, the wrapper must still
    // be able to run the original rather than returning 0 for every conversion.
    m_streamingHookInstalled = false;
    m_streamingTarget = nullptr;
}
