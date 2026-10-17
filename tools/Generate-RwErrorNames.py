#!/usr/bin/env python3
"""Regenerate Plugins/DebugUtils/DebugUtils.RwErrorNames.h.

RenderWare reports an error as an RwError {pluginID, errorCode}. When the MSB of
errorCode is set the code is one of the *common* errors, whose numeric value is
0x80000000 + index into the list built from the RenderWare errcom.def table. This
tool turns that list into a small constexpr table so the frame-time monitor can
print a name ("E_RW_NOMEM") instead of a raw number.

Plugin errors (MSB clear) are plugin specific; their code cannot be named without
the full plugin registry, so they are reported by pluginID + raw code instead and
are intentionally not in this table.

Source: tools/rw-error-names/errcom.def, copied verbatim from the RenderWare SDK
distribution shipped with DK22Pac/plugin-sdk (MIT). Do not edit by hand.

Run from the repository root:
    python tools/Generate-RwErrorNames.py
"""

import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "tools", "rw-error-names", "errcom.def")
OUT = os.path.join(ROOT, "Plugins", "DebugUtils", "DebugUtils.RwErrorNames.h")

# The common-error enum starts at E_RW_NOERROR = 0x80000000 and then splices in
# errcom.def, so entry i has value 0x80000000 + i.
BASE = 0x80000000


def load_names():
    names = []
    with open(SRC, encoding="utf-8", errors="replace") as handle:
        for line in handle:
            stripped = line.strip()
            if not stripped.startswith("RWECODE("):
                continue
            rest = stripped[len("RWECODE("):]
            names.append(rest.split(",")[0].strip())
    return names


def main():
    names = load_names()

    lines = [
        "// Generated from RenderWare errcom.def (RenderWare SDK, MIT).",
        "// Do not edit by hand; regenerate with tools/Generate-RwErrorNames.py.",
        "//",
        "// RwErrorCodeCommon values: 0x80000000 + index. Index 0 is E_RW_NOERROR.",
        "#pragma once",
        "",
        "namespace DebugUtilsRwErrorNames",
        "{",
        "    struct Entry { unsigned int code; const char* name; };",
        "    inline constexpr Entry kNames[] = {",
    ]
    lines.append(f'        {{ 0x{BASE:08X}, "E_RW_NOERROR" }},')
    for index, name in enumerate(names):
        lines.append(f'        {{ 0x{BASE + index + 1:08X}, "{name}" }},')
    lines += [
        "    };",
        "    inline constexpr unsigned kCount = sizeof(kNames) / sizeof(kNames[0]);",
        "}",
        "",
    ]

    with open(OUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines))

    print(f"wrote {len(names) + 1} RenderWare common error names to {OUT}")


if __name__ == "__main__":
    main()
