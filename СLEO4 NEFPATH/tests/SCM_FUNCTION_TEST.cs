{$CLEO .cs}

// ============================================================================
// CLEO 4.4.4 unofficial patch - Sanny Builder 3.8.5 0AB1 / 0AB2 function test
// GTA SA 1.0 US
// Opcode syntax source: opcodes SannyBuilder-v3.8.5(1).txt
//
// EXPECTED:
//   RESULT=30
//   C0=12345 C1=67890
//
// The caller locals 0@ and 1@ are deliberately initialized before 0AB1.
// The function overwrites them with its arguments (10 and 20), calculates
// 30, returns it through 4@, and ScmFunction::Return() must restore the
// caller's original 0@ / 1@ values.
//
// Exact Sanny Builder 3.8.5 spellings used here:
//   0AB1: cleo_call
//   0AB2: cleo_return
//
// No {$USE CLEO+} is required for this test.
// ============================================================================

thread "FTEST444"

wait 2000

0@ = 12345
1@ = 67890
4@ = -1

0AD1: show_formatted_text_highpriority "FUNCTEST START" time 2000
wait 500

0AB1: cleo_call @ADD_VALUES 2 10 20 4@

0AD1: show_formatted_text_highpriority "RESULT=%d C0=%d C1=%d" time 5000 4@ 0@ 1@
wait 6000

:LOOP
wait 1000
jump @LOOP

:ADD_VALUES
005A: 0@ += 1@
0AB2: cleo_return 1 0@
