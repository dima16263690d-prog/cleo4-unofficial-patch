# RenderFix

RenderFix remains a CLEO plugin (RenderFix.cleo) loaded and managed by CLEO 4 NEFPATH. It has two independent responsibilities; it is not a standalone ASI and does not replace CLEO.

## Two independent domains

### 1. RenderFix + GTA

Repairs and configuration that affect GTA San Andreas itself: frame pacing, high-FPS-sensitive behavior, renderer fixes, and streaming memory.

### 2. RenderFix + CLEO

A protective layer for drawing performed by CLEO plugins and scripts. The first implementation snapshots and restores RenderWare states commonly changed by CLEO+ sprite drawing: texture raster/address/filter, perspective, Z test/write, shade mode, blend modes, vertex alpha, fog and alpha-test state.

The state guard brackets the Plugin-SDK RenderWare drawing event, not CLEO's script-processing callbacks. Device loss/reset and RenderWare reinitialization invalidate snapshots so stale state is not replayed. It is configurable because a third-party graphics plugin may intentionally leave state changes for later code. It is a first safety layer, not a claim that every RenderWare or script error is fixed.

## Configuration

The file is read from the CLEO plugin configuration directory (or alongside the plugin if an INI is present). The master switch is off by default; both domains can then be controlled separately:

| Key | Default | Meaning |
| --- | --- | --- |
| [RenderFix.General] Enabled | 0 | Master switch. |
| [RenderFix.GTA] Enabled | 1 | Enable GTA-side fixes. |
| [RenderFix.CLEO] Enabled | 1 | Enable CLEO-side rendering protection. |
| [RenderFix.CLEO] RenderStateGuard | 1 | Snapshot/restore selected RenderWare states each game-process cycle. |
| [RenderFix.FPS] FrameLimit | 0 | Frame cap, 1..255; 0 = do not change current cap. |
| [RenderFix.FPS] RemoveFrameDelay | 0 | Remove approximately 14 ms frame delay. |
| [RenderFix.FPS] MinRefreshRate | 0 | Minimum refresh rate in Hz. |
| [RenderFix.AutoLimitFPS] Enabled | 0 | Temporarily lower the cap during selected activities. |
| [RenderFix.Overlay] ShowFPS | 0 | Show an FPS counter. |
| [RenderFix.CoreFixes] MouseReinitFix | 0 | Prevent repeated DirectInput mouse-device reinitialization. |
| [RenderFix.CoreFixes] RefFix | 0 | Prevent the reflection renderer divide-by-zero. |
| [RenderFix.CoreFixes] DoubleRwheelsFix | 0 | Correct RenderWare matrix flags for dual rear wheels. |
| [RenderFix.Streaming] MemoryMB | 0 | Request a minimum streaming budget in MB; -1 = automatic. |

The existing AutoLimitFPS activity keys and values remain available in the INI.

## Streaming memory ownership

RenderFix now writes the GTA 1.0 US CStreaming::ms_memoryAvailable field itself instead of delegating the request to CLEO_SetStreamingMemoryLimit. It applies the setting when fixes are initialized and checks the field in each game-process callback, so if GTA reinitializes streaming and lowers the value, RenderFix raises it again on the next callback. The unchanged path performs only a value check and no logging or protection changes. The requested value only raises the current budget; it never lowers a budget already set higher by the game or another mod. MemoryMB=-1 selects 1024 MB for a Large Address Aware executable, otherwise 512 MB.

## Scope and compatibility

- Supported build: GTA San Andreas Classic 1.0 US only. Address-based changes are skipped on other builds.
- Keep RenderFix.General.Enabled=0 to leave gameplay unchanged.
- Enable or disable the GTA and CLEO domains independently.
- The CLEO-side state guard is limited to selected RenderWare render states; resource lifetime tracking, device-reset recovery and broader script-render hooks remain future work.
- Compare with the guard enabled and disabled when using unusual graphics stacks.

## Existing patch-plan test

The address/value unit test remains at Plugins/RenderFix/Tests/renderfix_plan_test.cpp. It checks fixed constants and AutoLimitFPS rules without requiring GTA to run. The in-game modules still require a Windows build and runtime test in GTA SA 1.0 US.
