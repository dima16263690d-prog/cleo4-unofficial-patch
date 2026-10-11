#include "stdafx.h"
#include "CGameMenu.h"
#include "cleo.h"
#include "CDebugBridge.h"
#include <sstream>

namespace CLEO
{
    const char CLEO_VERSION_STR[] = VERSION_STRING;

    void(__cdecl\t* TextDraw)\t\t\t\t(float x, float y, const char* text);
    void(__cdecl\t* SetTextAlign)\t\t\t(BYTE nAlign);
    void(__cdecl\t* SetTextFont)\t\t\t(BYTE nFont);
    void(__cdecl* SetTextEdge)\t\t\t(char nEdge);
    void(__cdecl\t* SetLetterSize)\t\t\t(float w, float h);
    void(__cdecl\t* SetLetterColor)\t\t\t(RGBA color);

    // thiscalls dont work with memory_pointer operator overload template
    DWORD\tCMenuManager__ScaleX;
    DWORD\tCMenuManager__ScaleY;
    DWORD\tCTexture__DrawInRect;
    //void\t(__thiscall\t* CTexture__DrawInRect)\t(void *texture, RwRect2D *rect, RwRGBA color);

    CMenuManager *\tMenuManager;

    float CGameMenu_ScaleX(CMenuManager *pMenu, float w)
    {
        float fReturn;
        _asm
        {
            mov ecx, pMenu
            push w
            call CMenuManager__ScaleX
            fstp fReturn
        }
        return fReturn;
    }

    float CGameMenu_ScaleY(CMenuManager *pMenu, float w)
    {
        float fReturn;
        _asm
        {
            mov ecx, pMenu
            push w
            call CMenuManager__ScaleY
            fstp fReturn
        }
        return fReturn;
    }

    void CTexture_DrawInRect(void *pTexture, RwRect2D *rect, RwRGBA *colour)
    {
        _asm
        {
            push colour
            push rect
            mov ecx, pTexture
            call CTexture__DrawInRect
        }
    }

    void __fastcall OnDrawMenuBackground(void *texture, int dummy, RwRect2D *rect, RwRGBA *color)
    {
        CTexture_DrawInRect(texture, rect, color);
        auto cs_count = GetInstance().ScriptEngine.WorkingScriptsCount();
        auto plugin_count = GetInstance().PluginSystem.GetNumPlugins();
        std::ostringstream cleo_text;
        // Display the project branding in-game. Keep VERSION_STRING unchanged for API compatibility.
        cleo_text << "CLEO 4 NEFPATH";
#ifdef _DEBUG
        cleo_text << " DEBUG";
#endif
        SetTextAlign(1);
        SetTextFont(2);
        if (SetTextEdge) SetTextEdge(1);
        SetLetterSize(CGameMenu_ScaleX(MenuManager, 0.23f), CGameMenu_ScaleY(MenuManager, 0.4f));

        // Mint accent for the NEFPATH branding.
        SetLetterColor(RGBA(0xAD, 0xCE, 0xC4, 0xFF));
        TextDraw(CGameMenu_ScaleX(MenuManager, 6.0f), CGameMenu_ScaleY(MenuManager, 428.0f), cleo_text.str().c_str());
        cleo_text.str("");
        cleo_text.clear();

        if (cs_count || plugin_count)
        {
            if (plugin_count) cleo_text << plugin_count << (plugin_count > 1 ? " plugins" : " plugin");
            if (cs_count && plugin_count) cleo_text << " / ";
            if (cs_count) cleo_text << cs_count << (cs_count > 1 ? " scripts" : " script");
            //plugin_text << plugin_count << (plugin_count > 1 ? " plugins" : " plugin");
            SetTextAlign(1);
            //SetTextFont(2);

            SetLetterSize(CGameMenu_ScaleX(MenuManager, 0.18f), CGameMenu_ScaleY(MenuManager, 0.34f));

            SetLetterColor(RGBA(0xAD, 0xCE, 0xC4, 0xFF));
            TextDraw(CGameMenu_ScaleX(MenuManager, 6.0f), CGameMenu_ScaleY(MenuManager, 436.0f), cleo_text.str().c_str());
        }
    }

    void CGameMenu::Inject(CCodeInjector& inj)
    {
        TRACE("Injecting MenuStatusNotifier...");
        CGameVersionManager& gvm = GetInstance().VersionManager;
        MenuManager = gvm.TranslateMemoryAddress(MA_MENU_MANAGER);
        TextDraw = gvm.TranslateMemoryAddress(MA_DRAW_TEXT_FUNCTION);
        SetTextAlign = gvm.TranslateMemoryAddress(MA_SET_TEXT_ALIGN_FUNCTION);
        SetTextFont = gvm.TranslateMemoryAddress(MA_SET_TEXT_FONT_FUNCTION);
        SetTextEdge = gvm.TranslateMemoryAddress(MA_SET_TEXT_EDGE_FUNCTION);

        // Meh...
        CMenuManager__ScaleX = gvm.TranslateMemoryAddress(MA_CMENU_SCALE_X_FUNCTION);
        CMenuManager__ScaleY = gvm.TranslateMemoryAddress(MA_CMENU_SCALE_Y_FUNCTION);

        SetLetterSize = gvm.TranslateMemoryAddress(MA_SET_LETTER_SIZE_FUNCTION);
        SetLetterColor = gvm.TranslateMemoryAddress(MA_SET_LETTER_COLOR_FUNCTION);

        inj.MemoryReadOffset(gvm.TranslateMemoryAddress(MA_CALL_CTEXTURE_DRAW_BG_RECT).address + 1, CTexture__DrawInRect, true);
        //gvm.TranslateMemoryAddress(MA_CTEXTURE_DRAW_IN_RECT_FUNCTION);

        GetInstance().HookSystem.InstallCall(
            inj,
            "DrawMenuBackground",
            gvm.TranslateMemoryAddress(MA_CALL_CTEXTURE_DRAW_BG_RECT),
            (size_t)&OnDrawMenuBackground
        );
    }
}
