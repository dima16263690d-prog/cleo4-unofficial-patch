# RenderFix

RenderFix is the "fixer" companion to **DebugUtils**. DebugUtils only observes
(reports RenderWare errors, stutter, leaks); RenderFix optionally *changes*
frame pacing and streaming. Keeping them separate means the diagnostic plugin
never silently alters the thing it is measuring.

Every tweak is opt-in from `RenderFix.ini`, applied at game start, and logged to
`cleo\debug\cleo_renderfix.log`, so a change can be isolated and reverted
without rebuilding.

## Supported game

**GTA San Andreas 1.0 US only.** All addresses are 1.0 US specific. On any other
build the plugin logs that it did nothing and applies no patches.

## Settings (`cleo\cleo_plugins\RenderFix.ini`)

| Key | Default | Meaning |
| --- | --- | --- |
| `[RenderFix.General] Enabled` | 0 | Master switch. 0 = the plugin does nothing. |
| `[RenderFix.General] Log` | 1 | Write `cleo\debug\cleo_renderfix.log`. |
| `[RenderFix.FPS] FrameLimit` | 0 | FPS cap, 1..255. 0 = leave the engine value untouched. |
| `[RenderFix.FPS] RemoveFrameDelay` | 0 | Remove the engine's ~14 ms per-frame delay. |
| `[RenderFix.FPS] MinRefreshRate` | 0 | Minimum monitor refresh rate in Hz. 0 = untouched. |
| `[RenderFix.AutoLimitFPS] Enabled` | 0 | Lower the cap during high-FPS-breaking activities. |
| `[RenderFix.AutoLimitFPS] ForCutscenes` | 1 | Cutscene cap 60. |
| `[RenderFix.AutoLimitFPS] ForScriptedScenes` | 1 | Scripted-scene cap 80. |
| `[RenderFix.AutoLimitFPS] ForPauseMenu` | 1 | Pause-menu cap 60. |
| `[RenderFix.AutoLimitFPS] ForMinigames` | 1 | Minigame cap 30 (`POOL2`, `GFSEX`). |
| `[RenderFix.AutoLimitFPS] ForMissions` | 1 | Mission-interior cap 50 (`DRUGS1` inside). |
| `[RenderFix.AutoLimitFPS] ForSchools` | 1 | School cap 80 (`DSKOOL`, `BOAT`, `BSKOOL`). |
| `[RenderFix.Streaming] MemoryMB` | 0 | Streaming budget in MB. 0 = untouched, -1 = auto, >0 = that many MB. |

## What each tweak does

- **FrameLimit** writes the engine's frame-limiter byte (`0xC1704C`). It is
  re-applied once on the first processed frame because the engine re-initialises
  the limiter shortly after the scripts start (the same approach SA-MixSets and
  FramerateVigilante use).
- **RemoveFrameDelay** removes the engine's ~14 ms per-frame sleep
  (SilentPatch's "No framedelay"). This is what usually unlocks a smooth high
  frame rate.
- **MinRefreshRate** writes the immediate operand of the refresh-rate instruction
  at `0x74612C` (FramerateVigilante's "RefreshRate").
- **AutoLimitFPS** ports FramerateVigilante's automatic limiter: every frame it
  reads the cutscene / scripted-scene / pause flags and walks the active script
  list, and *lowers* the cap only while an activity that breaks at high FPS runs.
  The override can only lower the base `FrameLimit`, never raise it, and it is
  written only when the value changes so it does not fight another mod per frame.
  This is the safest way to run at 60+ FPS: you keep the smooth cap everywhere
  except the few places the engine needs 30.
- **MemoryMB** is handed to the CLEO core (`CLEO_SetStreamingMemoryLimit`) and
  applied at SCM init, so the engine's own streaming initialisation cannot
  clobber it. The value is only ever raised, never lowered.

## Roadmap — what could be added next (Tier 2)

SilentPatch and FramerateVigilante carry far more than frame pacing. The
high-value, low-risk items to port next, in rough order:

1. **Physics-at-high-FPS fixes (FramerateVigilante).** These are the real reason
   60+ FPS feels wrong: swim/dive speed scaling, buoyancy cutoff, swing-door
   damping, siren toggle, helicopter rotor speed, burnout, ped-push-car force.
   Each is a small inline hook, but all are *behaviour* patches, so they belong
   behind their own ini key and should be tested one at a time.
2. **SilentPatch "No framedelay" variants and Timer fixes.** SilentPatch also
   fixes `CTimer` float→int rounding; useful above 30 FPS.
3. **Streaming / memory (MixSets territory).** `MemoryMB` is already here; pool
   sizes and a "load whole map" mode are the MixSets features worth considering,
   but they overlap with other mods and are riskier.

Everything above is opt-in and off by default, so RenderFix stays a no-op unless
the user asks for a specific change.

## Caveats

- Raising the frame rate above 30 FPS can expose physics and timing issues the
  engine was not designed for. Enable one tweak at a time and test.
- The addresses come from SilentPatch, SA-MixSets and FramerateVigilante (all
  MIT). See `RenderFix.Patch.h` for the exact values and their sources.

## Tests

The patch plan (addresses and bytes) has a host unit test, because a wrong byte
silently corrupts the game's code:

```
g++ -std=c++17 -I Plugins/RenderFix Plugins/RenderFix/Tests/renderfix_plan_test.cpp -o /tmp/renderfix_plan_test
/tmp/renderfix_plan_test
```
