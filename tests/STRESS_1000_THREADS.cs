{$CLEO .cs}

// ============================================================================
// CLEO 4.4.4 unofficial patch - script processing stress test
// GTA SA 1.0 US, Sanny Builder 3.8.5, requires CLEOPlus.cleo (0E6F)
//
// Starts 1000 child custom-scripts with 0E6F. Each child sleeps in
// "wait 1000", so it exercises the sleeping fast path of
// CCustomScript::Process(). The parent shows the thread count and FPS.
//
// EXPECTED:
//   THREADS=1000 FPS=<close to the FPS without this script>
//
// To stress the active path instead, change "wait 1000" in CHILD_LOOP
// to "wait 0".
// ============================================================================

thread "STRESS1K"

wait 3000

0@ = 0

:SPAWN_LOOP
0E6F: stream_custom_script_from_label @CHILD 0@
000A: 0@ += 1
0019:   0@ > 999
004D: jump_if_false @SPAWN_LOOP

:SHOW
wait 500
0A8D: 10@ = read_memory 0xB7CB50 size 4 virtual_protect 0   // CTimer::game_FPS
0092: 11@ = float 10@ to_integer
0AD1: show_formatted_text_highpriority "THREADS=%d FPS=%d" time 600 0@ 11@
jump @SHOW

:CHILD
// 0@ = child index passed by 0E6F
1@ = 0

:CHILD_LOOP
wait 1000
000A: 1@ += 1
jump @CHILD_LOOP
