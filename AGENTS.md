# AGENTS.md

Guidance for agents (and humans) working in this repository.

## What this project is

**CLEO 4.4.4 Unofficial Patch** — an unofficial continuation of CLEO 4.4.4 for
GTA San Andreas. The core is a 32-bit ASI (`CLEO4.vcxproj` → `output\Release\CLEO.asi`)
that hosts script plugins built as `.cleo` DLLs. See `UPSTREAM.md` for the
relationship to official CLEO 4 / CLEO 5.

## Target and toolchain

- **Win32 (x86), MSVC, GTA SA 1.0 US.** This is the only supported target.
- The build is a Visual Studio solution: `CLEO4.sln`, configurations
  `Debug GTASA|Win32` and `Release GTASA|Win32`.
- Core project: `CLEO4.vcxproj` (targets `CLEO.asi`). Plugins live under
  `Plugins\<Name>\` (e.g. `DebugUtils`, `RenderFix`) and target `.cleo` via
  `<TargetExt>.cleo</TargetExt>`.
- Plugins link `cleo.lib` (from `cleo_sdk\`) and reference the core project.
  The plugin-sdk include paths come from `$(PLUGIN_SDK_DIR)` (see each
  `.vcxproj`).

There is **no native Linux build**. On Linux the only verification available is
a mingw-w64 syntax check (below). The final build must be done on Windows.

## Verifying on Linux (syntax check only)

`i686-w64-mingw32-g++-posix` plus shim headers under `/workspace/shim`,
`/workspace/winshim` let the translation units be parsed without Windows:

```
/workspace/check.sh <file.cpp> [core|plugin]
```

- `plugin` (default) is for files under `Plugins\...`.
- `core` is for files under `source\`.

It runs `-fsyntax-only`, so it catches typos, missing declarations and type
errors, but it does **not** link or run anything. The equivalent raw command for
a plugin file is:

```
i686-w64-mingw32-g++-posix -std=c++23 -fsyntax-only -w -DRW -DGTASA -D_WIN32_WINNT=0x0501 \
  -I <winshim> -I Plugins/DebugUtils -I cleo_sdk \
  -I <plugin-sdk>/plugin_sa -I <plugin-sdk>/plugin_sa/game_sa \
  -I <plugin-sdk>/plugin_sa/game_sa/enums -I <plugin-sdk>/plugin_sa/game_sa/rw \
  -I <plugin-sdk>/shared -I <plugin-sdk>/shared/game \
  <file.cpp>
```

## Host unit tests

Logic that has no Windows dependency is tested on the host with plain `g++`.
Existing tests:

- `Plugins/DebugUtils/Tests/x86_decoder_test.cpp` — JMP-detour prologue decoder.
- `Plugins/RenderFix/Tests/renderfix_plan_test.cpp` — the raw byte patches
  RenderFix applies to the game.

Run one like:

```
g++ -std=c++17 -I Plugins/RenderFix Plugins/RenderFix/Tests/renderfix_plan_test.cpp -o /tmp/t && /tmp/t
```

Exit code 0 = all checks passed. Keep any address/byte table in a header that
the test can include, so the test and the plugin share one source of truth.

## Conventions

- C++23, MSVC (`/std:c++latest` equivalent via `<LanguageStandard>stdcpp23`).
- Keep game addresses in one place per plugin and mark them 1.0 US specific.
  Guard version-specific code with `CLEO_GetGameVersion()`.
- Diagnostic plugins must not silently change what they measure. Observation
  (logging, reporting) and modification (patching) belong in separate plugins —
  e.g. `DebugUtils` observes, `RenderFix` patches.
- Comments explain *why*, not *what*. Do not narrate the change history.

## Pull requests

- Base branch: `main` (or the current integration branch, e.g. `s101tst01`).
  Never push directly to `main`.
- Update the existing PR branch instead of opening a new PR per iteration.
- PR descriptions may be written in Russian to match the requester; code,
  comments and identifiers stay in English.
