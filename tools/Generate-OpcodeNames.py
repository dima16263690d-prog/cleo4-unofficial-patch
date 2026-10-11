#!/usr/bin/env python3
"""Regenerate Plugins/DebugUtils/DebugUtils.OpcodeNames.h.

The opcode name table merges two upstream sources so the crash report can
print a human readable opcode name next to the raw number:

  * tools/opcode-names/scrlog-keywords.txt
      scrlog keywords.txt (LINK/2012, MIT) - the vanilla GTA SA opcode names.
  * tools/opcode-names/cleoplus-opcodes.txt
      CLEO+ "opcodes.txt" (JuniorDjjr) - CLEO opcodes 0Dxx/0Exx that the
      vanilla list does not cover.

Run from the repository root:
    python tools/Generate-OpcodeNames.py
"""

import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC_DIR = os.path.join(ROOT, "tools", "opcode-names")
OUT = os.path.join(ROOT, "Plugins", "DebugUtils", "DebugUtils.OpcodeNames.h")

IDENT = re.compile(r"[A-Za-z0-9_]+")
CLEO_LINE = re.compile(r"^([0-9A-Fa-f]{4}):\s*([A-Za-z0-9_]+)")


def load_names():
    names = {}

    with open(os.path.join(SRC_DIR, "scrlog-keywords.txt"), encoding="utf-8", errors="replace") as handle:
        for line in handle:
            line = line.strip()
            if "=" not in line:
                continue
            opcode, name = line.split("=", 1)
            try:
                value = int(opcode, 16)
            except ValueError:
                continue
            name = name.strip()
            if value not in names and IDENT.fullmatch(name):
                names[value] = name

    with open(os.path.join(SRC_DIR, "cleoplus-opcodes.txt"), encoding="utf-8", errors="replace") as handle:
        for line in handle:
            match = CLEO_LINE.match(line.strip())
            if not match:
                continue
            value = int(match.group(1), 16)
            names.setdefault(value, match.group(2).upper())

    return names


def main():
    names = load_names()

    lines = [
        "// Generated from scrlog keywords.txt (LINK/2012, MIT) and CLEO+ opcodes_txt (JuniorDjjr).",
        "// Do not edit by hand; regenerate with tools/Generate-OpcodeNames.py.",
        "#pragma once",
        "",
        "namespace DebugUtilsOpcodeNames",
        "{",
        "    struct Entry { unsigned short opcode; const char* name; };",
        "    inline constexpr Entry kNames[] = {",
    ]
    for opcode, name in sorted(names.items()):
        lines.append(f'        {{ 0x{opcode:04X}, "{name}" }},')
    lines += [
        "    };",
        "    inline constexpr unsigned kCount = sizeof(kNames) / sizeof(kNames[0]);",
        "}",
        "",
    ]

    with open(OUT, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(lines))

    print(f"wrote {len(names)} opcode names to {OUT}")


if __name__ == "__main__":
    main()
