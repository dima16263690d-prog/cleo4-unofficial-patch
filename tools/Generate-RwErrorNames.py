#!/usr/bin/env python3
"""Regenerate Plugins/DebugUtils/DebugUtils.RwErrorNames.h.

RenderWare reports an error as an RwError {pluginID, errorCode}. Two name tables
are produced:

  * common errors (errcom.def). These are the error codes whose MSB is set; their
    numeric value is 0x80000000 + index into the errcom.def list, whose index 0 is
    E_RW_NOERROR.

  * core-plugin errors (errcore.def). When the MSB is clear the code belongs to a
    plugin; the plugin id is in the pair's pluginID. The RenderWare core plugin
    (pluginID 0x01, rwID_COREPLUGIN) is the one every game links in, so its codes
    are the common "plugin error" values seen in practice. Its codes are the
    zero-based index into errcore.def (e.g. code 0x16 = E_RW_READTEXMASK).

Plugin errors from other pluginIDs cannot be named without the full plugin
registry, so they are reported by pluginID + raw code instead.

Sources (copied verbatim from the RenderWare SDK distribution shipped with
DK22Pac/plugin-sdk, MIT): tools/rw-error-names/errcom.def, errcore.def.
Do not edit this header by hand.

Run from the repository root:
    python tools/Generate-RwErrorNames.py
"""

import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(ROOT, "tools", "rw-error-names")
OUT = os.path.join(ROOT, "Plugins", "DebugUtils", "DebugUtils.RwErrorNames.h")

# The common-error enum starts at E_RW_NOERROR = 0x80000000 and then splices in
# errcom.def, so entry i has value 0x80000000 + i.
BASE = 0x80000000

# RenderWare core plugin id (rwID_COREPLUGIN); owns errcore.def.
CORE_PLUGIN_ID = 0x01


def load_entries(path):
    """Return (name, text) pairs from a RenderWare RWECODE .def file.

    Each entry is RWECODE(NAME,\n "message" ...) and the message may span several
    quoted lines; they are joined with spaces and newlines flattened.
    """
    entries = []
    with open(path, encoding="utf-8", errors="replace") as handle:
        text = handle.read()

    for chunk in text.split("RWECODE(")[1:]:
        name = chunk.split(",", 1)[0].strip()
        rest = chunk.split(",", 1)[1]
        # Collect every double-quoted fragment up to the closing parenthesis of
        # this RWECODE; join them and flatten whitespace.
        parts = []
        for piece in rest.split('"')[1::2]:
            parts.append(piece)
        message = " ".join(parts)
        message = " ".join(message.split())
        message = message.replace("\\", "\\\\").replace('"', '\\"')
        entries.append((name, message))
    return entries


def emit_entries(lines, entries, value_of, indent):
    for index, (name, message) in enumerate(entries):
        lines.append(
            f'{indent}{{ {value_of(index)}, "{name}", "{message}" }},'
        )


def main():
    common = load_entries(os.path.join(SRC_DIR, "errcom.def"))
    core = load_entries(os.path.join(SRC_DIR, "errcore.def"))

    lines = [
        "// Generated from RenderWare errcom.def / errcore.def (RenderWare SDK, MIT).",
        "// Do not edit by hand; regenerate with tools/Generate-RwErrorNames.py.",
        "//",
        "// RwErrorCodeCommon: code = 0x80000000 + index (index 0 is E_RW_NOERROR).",
        "// RwErrorCodePlugin_errcore: code = index into errcore.def, for the core",
        "// plugin (pluginID 0x01).",
        "#pragma once",
        "",
        "namespace DebugUtilsRwErrorNames",
        "{",
        "    struct Entry { unsigned int code; const char* name; const char* text; };",
        "",
        "    // Common errors (errorCode has the MSB set).",
        "    inline constexpr Entry kNames[] = {",
    ]
    lines.append('        { 0x%08X, "E_RW_NOERROR", "No error" },' % BASE)
    emit_entries(
        lines,
        common,
        lambda i: "0x%08X" % (BASE + i + 1),
        "        ",
    )
    lines += [
        "    };",
        "    inline constexpr unsigned kCount = sizeof(kNames) / sizeof(kNames[0]);",
        "",
        "    // RenderWare core plugin (pluginID 0x01) errors: code = index.",
        "    inline constexpr unsigned int kCorePluginId = 0x01;",
        "    inline constexpr Entry kCoreNames[] = {",
    ]
    emit_entries(lines, core, lambda i: "0x%04X" % i, "        ")
    lines += [
        "    };",
        "    inline constexpr unsigned kCoreCount =",
        "        sizeof(kCoreNames) / sizeof(kCoreNames[0]);",
        "}",
        "",
    ]

    with open(OUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines))

    print(
        f"wrote {len(common) + 1} common + {len(core)} core-plugin names to {OUT}"
    )


if __name__ == "__main__":
    main()
