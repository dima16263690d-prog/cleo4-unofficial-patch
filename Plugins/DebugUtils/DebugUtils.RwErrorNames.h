// Generated from RenderWare errcom.def (RenderWare SDK, MIT).
// Do not edit by hand; regenerate with tools/Generate-RwErrorNames.py.
//
// RwErrorCodeCommon values: 0x80000000 + index. Index 0 is E_RW_NOERROR.
#pragma once

namespace DebugUtilsRwErrorNames
{
    struct Entry { unsigned int code; const char* name; };
    inline constexpr Entry kNames[] = {
        { 0x80000000, "E_RW_NOERROR" },
        { 0x80000001, "E_RW_BADENGINESTATE" },
        { 0x80000002, "E_RW_BADOPEN" },
        { 0x80000003, "E_RW_BADPARAM" },
        { 0x80000004, "E_RW_BADVERSION" },
        { 0x80000005, "E_RW_DEBUGSTACK" },
        { 0x80000006, "E_RW_DEFAULTPIPELINECREATION" },
        { 0x80000007, "E_RW_FRAMENOMATRIX" },
        { 0x80000008, "E_RW_INVIMAGEDEPTH" },
        { 0x80000009, "E_RW_INVIMAGEFORMAT" },
        { 0x8000000A, "E_RW_INVIMAGEMASK" },
        { 0x8000000B, "E_RW_INVIMAGESIZE" },
        { 0x8000000C, "E_RW_INVRASTERDEPTH" },
        { 0x8000000D, "E_RW_INVRASTERFORMAT" },
        { 0x8000000E, "E_RW_INVRASTERLOCKREQ" },
        { 0x8000000F, "E_RW_INVRASTERMIPLEVEL" },
        { 0x80000010, "E_RW_INVRASTERSIZE" },
        { 0x80000011, "E_RW_INVRASTERUNLOCKREQ" },
        { 0x80000012, "E_RW_NOFRAME" },
        { 0x80000013, "E_RW_NOMEM" },
        { 0x80000014, "E_RW_NOMIPMAPGENERATIONCALLBACK" },
        { 0x80000015, "E_RW_NOTSSEENABLEDCPU" },
        { 0x80000016, "E_RW_NULLP" },
        { 0x80000017, "E_RW_PLUGININIT" },
        { 0x80000018, "E_RW_PLUGINNOTINIT" },
        { 0x80000019, "E_RW_RANGE" },
        { 0x8000001A, "E_RW_READ" },
        { 0x8000001B, "E_RW_REDUNDANT_FUNCTION" },
        { 0x8000001C, "E_RW_WRITE" },
        { 0x8000001D, "E_RX_MESHES_RANGES_OVERLAP" },
        { 0x8000001E, "E_RW_STRING_TRUNCATION" },
    };
    inline constexpr unsigned kCount = sizeof(kNames) / sizeof(kNames[0]);
}
