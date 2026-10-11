#include "stdafx.h"
#include "CCustomScript.h"
#include "cleo.h"
#include "crc32.h"
#include "ScmFunction.h"
#include "CCleoMemoryManager.h"
#include <cstring>

namespace CLEO
{
    extern CTexture* scriptSprites;
    extern BYTE* scriptDraws;
    extern WORD* numScriptDraws;
    extern WORD* numScriptTexts;
    extern BYTE* useTextCommands;
    extern BYTE* scriptTexts;
    extern void(__cdecl* DrawScriptStuff)(char bBeforeFade);
    extern void(__cdecl* DrawScriptStuff_H)(char bBeforeFade);
    extern CCustomScript* lastScriptCreated;
    extern DWORD* GameTimer;
    extern BYTE* MissionLoaded;
    extern void RunScriptDeleteDelegate(CRunningScript* script);

#define NUM_STORED_SPRITES 128
#define NUM_STORED_DRAWS 128
#define NUM_STORED_TEXTS 96
#define DRAW_DATA_SIZE 60
#define TEXT_DATA_SIZE 68
#define DRAW_ARRAY_SIZE NUM_STORED_DRAWS*DRAW_DATA_SIZE
#define TEXT_ARRAY_SIZE NUM_STORED_TEXTS*TEXT_DATA_SIZE
    CTexture storedSprites[NUM_STORED_SPRITES];
    BYTE storedDraws[DRAW_ARRAY_SIZE];
    BYTE storedTexts[TEXT_ARRAY_SIZE];
    BYTE storedUseTextCommands = 0;
    WORD numStoredDraws = 0;
    WORD numStoredTexts = 0;

    static void FillTextDrawDefaults(BYTE* texts)
    {
        for (int i = 0; i<NUM_STORED_TEXTS; ++i)
        {
            CTextDrawer * pText = (CTextDrawer*)&texts[i*TEXT_DATA_SIZE];
            pText->m_fScaleX = 0.48f;
            pText->m_fScaleY = 1.12f;
            pText->m_Colour = CRGBA(0xE1, 0xE1, 0xE1, 0xFF);
            pText->m_bJustify = false;
            pText->m_bAlignRight = false;
            pText->m_bCenter = false;
            pText->m_bBackground = false;
            pText->m_bUnk1 = false;
            pText->m_fLineHeight = 182.0f;
            pText->m_fLineWidth = 640.0f;
            pText->m_BackgroundColour = CRGBA(0x80, 0x80, 0x80, 0x80);
            pText->m_bProportional = true;
            pText->m_EffectColour = CRGBA(0, 0, 0, 0xFF);
            strncpy(pText->m_szGXT, "", 8);
            pText->m_ucShadow = 2;
            pText->m_ucOutline = 0;
            pText->m_bDrawBeforeFade = false;
            pText->m_nFont = 1;
            pText->m_fPosX = 0.0;
            pText->m_fPosY = 0.0;
            pText->m_nParam1 = -1;
            pText->m_nParam2 = -1;
        }
    }

    static void RestoreTextDrawDefaults()
    {
        // Called up to twice per processed script every frame, so copy a
        // prebuilt block instead of setting every field of all 96 entries.
        static BYTE defaultTexts[TEXT_ARRAY_SIZE] = {};
        static bool defaultTextsReady = false;
        if (!defaultTextsReady)
        {
            FillTextDrawDefaults(defaultTexts);
            defaultTextsReady = true;
        }
        std::memcpy(scriptTexts, defaultTexts, TEXT_ARRAY_SIZE);
    }


    void CCustomScript::Process()
    {
        // A sleeping non-mission script executes no commands this frame
        // (GTA's ProcessScript only runs it once GameTimer >= WakeTime), so
        // skip the draw/texture swap and apply only the per-frame text reset.
        if (!bIsMission && !bUseMissionCleanup && !bWastedBustedCheck && GameTimer && *GameTimer < WakeTime)
        {
            if (resources.UseTextCommands)
            {
                const bool hadDrawableResources =
                    !resources.script_draws.empty() || !resources.script_texts.empty();

                resources.script_draws.clear();
                resources.script_texts.clear();
                resources.NumDraws = 0;
                resources.NumTexts = 0;
                if (resources.UseTextCommands == 1)
                    resources.UseTextCommands = 0;

                if (hadDrawableResources)
                    GetInstance().ScriptEngine.UpdateDrawableScript(this, false);
            }
            return;
        }

        RestoreScriptSpecifics();

        bool bNeedDefaults = false;
        if (*useTextCommands)
        {
            RestoreTextDrawDefaults();
            *numScriptTexts = 0;
            std::fill(scriptDraws, scriptDraws + DRAW_ARRAY_SIZE, 0);
            *numScriptDraws = 0;
            if (*useTextCommands == 1)
                *useTextCommands = 0;
        }
		
		ProcessScript(this);

        StoreScriptSpecifics();
    }
    void CCustomScript::Draw(char bBeforeFade)
    {
        // no point if this script doesn't draw
        if (resources.script_draws.size() || resources.script_texts.size())
        {
            static CCustomScript * last;
            last = this;
            RestoreScriptDraws();
            RestoreScriptTextures();
            if (bBeforeFade) DrawScriptStuff_H(bBeforeFade);
            else DrawScriptStuff(bBeforeFade);
            StoreScriptDraws();
            StoreScriptTextures();
        }
    }
    void CCustomScript::StoreScriptDraws()
    {
        // store this scripts draws + texts
        const bool hadDrawableResources =
            !resources.script_draws.empty() || !resources.script_texts.empty();

        if (*numScriptDraws)
            resources.script_draws.assign(scriptDraws, scriptDraws + (*numScriptDraws * DRAW_DATA_SIZE));
        else if (resources.script_draws.size())
            resources.script_draws.clear();
        if (*numScriptTexts)
            resources.script_texts.assign(scriptTexts, scriptTexts + (*numScriptTexts * TEXT_DATA_SIZE));
        else if (resources.script_texts.size())
            resources.script_texts.clear();

        resources.UseTextCommands = *useTextCommands;
        resources.NumDraws = *numScriptDraws;
        resources.NumTexts = *numScriptTexts;

        const bool hasDrawableResources =
            !resources.script_draws.empty() || !resources.script_texts.empty();
        if (hadDrawableResources != hasDrawableResources)
            GetInstance().ScriptEngine.UpdateDrawableScript(this, hasDrawableResources);

        // restore SCM draws + texts
        if (numStoredDraws) std::copy(storedDraws, storedDraws + (numStoredDraws * DRAW_DATA_SIZE), scriptDraws);
        else std::fill(scriptDraws, scriptDraws + DRAW_ARRAY_SIZE, 0);
        if (numStoredTexts) std::copy(storedTexts, storedTexts + (numStoredTexts * TEXT_DATA_SIZE), scriptTexts);
        else RestoreTextDrawDefaults();
        *numScriptDraws = numStoredDraws;
        *numScriptTexts = numStoredTexts;
        *useTextCommands = storedUseTextCommands;
    }
    void CCustomScript::RestoreScriptDraws()
    {
        // store SCM draws + texts
        storedUseTextCommands = *useTextCommands;
        numStoredDraws = *numScriptDraws;
        numStoredTexts = *numScriptTexts;
        if (numStoredDraws)
            std::copy(scriptDraws, scriptDraws + (numStoredDraws *  DRAW_DATA_SIZE), storedDraws);
        if (numStoredTexts)
            std::copy(scriptTexts, scriptTexts + (numStoredTexts * TEXT_DATA_SIZE), storedTexts);

        // restore script draws + texts
        if (!resources.script_draws.size()) *numScriptDraws = 0;
        else
        {
            std::copy(resources.script_draws.begin(), resources.script_draws.end(), scriptDraws);
            *numScriptDraws = resources.NumDraws;
        }
        if (!resources.script_texts.size()) *numScriptTexts = 0;
        else
        {
            std::copy(resources.script_texts.begin(), resources.script_texts.end(), scriptTexts);
            *numScriptTexts = resources.NumTexts;
        }
        *useTextCommands = resources.UseTextCommands;
    }
    void CCustomScript::StoreScriptTextures()
    {
        // store this scripts textures + restore SCM textures + make sure this scripts textures arent cleared by another
        // Reuse the list nodes instead of reallocating all of them every frame.
        if (resources.script_textures.size() != NUM_STORED_SPRITES)
            resources.script_textures.resize(NUM_STORED_SPRITES);
        auto texture = resources.script_textures.begin();
        for (int i = 0; i<NUM_STORED_SPRITES; ++i, ++texture)
        {
            *texture = *(RwTexture**)&scriptSprites[i];
            scriptSprites[i] = storedSprites[i];
        }

        //std::copy(scriptSprites, scriptSprites + NUM_STORED_SPRITES, storedSprites);
    }
    void CCustomScript::RestoreScriptTextures()
    {
        int n = 0;

        // store SCM textures
        for (int i = 0; i<NUM_STORED_SPRITES; ++i)
        {
            storedSprites[i] = scriptSprites[i];
        }
        //std::copy(scriptSprites, scriptSprites + NUM_STORED_SPRITES, storedSprites);

        // ensure SCM textures arent cleared - except by the SCM
        if (!resources.script_textures.size())
            std::fill((RwTexture**)scriptSprites, (RwTexture**)scriptSprites + NUM_STORED_SPRITES, nullptr);
        else
        {
            // restore textures for this script
            for (auto i = resources.script_textures.begin(); i != resources.script_textures.end(); ++i, ++n)
            {
                if (n >= NUM_STORED_SPRITES) break;
                *(RwTexture**)(&scriptSprites[n]) = *i;
            }
        }
    }
    void CCustomScript::StoreScriptSpecifics()
    {
        StoreScriptDraws();
        StoreScriptTextures();
    }
    void CCustomScript::RestoreScriptSpecifics()
    {
        RestoreScriptDraws();
        RestoreScriptTextures();
    }



    CCustomScript::CCustomScript(const char *szFileName, bool bIsMiss, CCustomScript *parent, int label)
        : CRunningScript(), cleoState(), parentThread(nullptr), childLabel(label), childThreads(), resources()    {
        IsCustom(1);
        bIsMission = bUseMissionCleanup = bIsMiss;
        cleoState.CompatVer = CLEO_VERSION;
        resources.UseTextCommands = 0;
        resources.NumDraws = 0;
        resources.NumTexts = 0;

        TRACE("Loading custom script %s...", szFileName);

        try
        {
			std::ifstream is;
			if (label != 0) // Create external from label.
			{
				if (!parent)
					throw std::logic_error("Trying to create external thread from label without parent thread");
				// Child custom scripts may only be created from another custom script.
				// This keeps 0E6F/CLEO_CreateCustomScript from treating a native SCM
				// thread as a CCustomScript and using an incompatible code buffer.
				if (!parent->IsCustom())
					throw std::logic_error("Trying to create external thread from non-custom parent thread");
				// Child scripts inherit the parent's CLEO compatibility mode.
				cleoState.CompatVer = parent->GetCompatibility();
				BaseIP = parent->GetBasePointer();
				CurrentIP = parent->GetBasePointer() - label;
								cleoState.CodeSize = parent->GetCodeSize();
				cleoState.ScriptFileDir = parent->GetScriptFileDir();
				cleoState.ScriptFileName = parent->GetScriptFileName();
memcpy(Name, parent->Name, sizeof(Name));
				cleoState.dwChecksum = parent->cleoState.dwChecksum;
				parentThread = parent;
				parent->childThreads.push_back(this);
			}
			else
			{
				using std::ios;
				std::ifstream is(szFileName, std::ios::binary);
				is.exceptions(std::ios::badbit | std::ios::failbit);
				std::size_t length;
				is.seekg(0, std::ios::end);
				length = static_cast<std::size_t>(is.tellg());
				cleoState.CodeSize = length;
				is.seekg(0, std::ios::beg);

				if (bIsMiss)
				{
					if (*MissionLoaded)
						throw std::logic_error("Starting of custom mission when other mission loaded");
					*MissionLoaded = 1;
					BaseIP = CurrentIP = missionBlock;
				}
				else {
					cleoState.ownedBuffer = new BYTE[length];
					BaseIP = CurrentIP = cleoState.ownedBuffer;
				}
				is.read(reinterpret_cast<char *>(BaseIP), length);

				const char *fname = strrchr(szFileName, '\\');
				const char *slash = strrchr(szFileName, '/');
				const char *sep = nullptr;
				if (slash && (!fname || slash > fname)) sep = slash;
				else sep = fname;

				if (sep)
				{
					cleoState.ScriptFileDir.assign(szFileName, static_cast<size_t>(sep - szFileName));
					fname = sep + 1;
				}
				else
				{
					cleoState.ScriptFileDir.clear();
					fname = szFileName;
				}

				cleoState.ScriptFileName = fname;
				memcpy(Name, fname, sizeof(Name));
				Name[7] = '\0';
				cleoState.dwChecksum = crc32(reinterpret_cast<BYTE *>(BaseIP), length);
			}
			lastScriptCreated = this;
            cleoState.bOK = true;
            if (parent)
            {
            }
            else
            {
            }
        }
        catch (std::exception& e)
        {
            TRACE("Error during loading of custom script %s occured.\nError message: %s", szFileName, e.what());
        }
        catch (...)
        {
            TRACE("Unknown error during loading of custom script %s occured.", szFileName);
        }
    }

    CCustomScript::~CCustomScript()
    {
        ScmFunction::ReleaseForScript(this);
        GetSmartMemoryEngine().Memory().ReleaseOwner(this);

        if (parentThread)
        {
            parentThread->childThreads.remove(this);
            parentThread = nullptr;
        }

        if (cleoState.ownedBuffer)
            delete[] cleoState.ownedBuffer;

		RunScriptDeleteDelegate(reinterpret_cast<CRunningScript*>(this));
		if (lastScriptCreated == this) lastScriptCreated = nullptr;
    }

}
