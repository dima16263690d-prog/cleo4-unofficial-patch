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
| `[RenderFix.Streaming] MemoryMB` | 0 | Streaming budget in MB. 0 = untouched, -1 = auto, >0 = that many MB. |

## What each tweak does

- **FrameLimit** writes the engine's frame-limiter byte (`0xC1704C`). It is
  re-applied once on the first processed frame because the engine re-initialises
  the limiter shortly after the scripts start (the same approach SA-MixSets and
  FramerateVigilante use).
- **RemoveFrameDelay** removes the engine's ~14 ms per-frame sleep
  (SilentPatch's "No framedelay"). This is what usually unlocks a smooth high
  frame rate.
- **MemoryMB** is handed to the CLEO core (`CLEO_SetStreamingMemoryLimit`) and
  applied at SCM init, so the engine's own streaming initialisation cannot
  clobber it. The value is only ever raised, never lowered.

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
