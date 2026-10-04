{$CLEO .cs}

//
// CLEO 4.4.4 unofficial patch
// 0AB1 / 0AB2 ConditionResult test
// GTA SA 1.0 US
//
// Проверяет:
//   1) 0AB1 -> функция возвращает TRUE -> if and
//   2) 0AB1 -> функция возвращает FALSE -> if and
//   3) 0AB1 -> функция возвращает FALSE -> if or
//   4) 0AB1 -> функция возвращает TRUE -> if or
//   5) NOT над результатом 0AB1
//
// ВАЖНО:
//   0E6F / CCustomScript / parentThread / childThreads этот тест НЕ использует.
//   Проверяется только ConditionResult механизма ScmFunction.
//
// EXPECTED:
//   AND_TRUE=PASS
//   AND_FALSE=PASS
//   OR_FALSE=PASS
//   OR_TRUE=PASS
//   NOT_TRUE=PASS
//

thread "CFNCTEST"

wait 2000

0@ = 1

// ------------------------------------------------------------
// TEST 1: function returns TRUE, followed by TRUE => AND TRUE
// ------------------------------------------------------------

if and
    0AB1: cleo_call @COND_TRUE 0
    0@ == 1
then
    0AD1: show_formatted_text_highpriority "AND_TRUE=PASS" time 2000
else
    0AD1: show_formatted_text_highpriority "AND_TRUE=FAIL" time 2000
end

wait 2500

// ------------------------------------------------------------
// TEST 2: function returns FALSE, followed by TRUE => AND FALSE
// ------------------------------------------------------------

if and
    0AB1: cleo_call @COND_FALSE 0
    0@ == 1
then
    0AD1: show_formatted_text_highpriority "AND_FALSE=FAIL" time 2000
else
    0AD1: show_formatted_text_highpriority "AND_FALSE=PASS" time 2000
end

wait 2500

0@ = 0

// ------------------------------------------------------------
// TEST 3: function returns FALSE, followed by FALSE => OR FALSE
// ------------------------------------------------------------

if or
    0AB1: cleo_call @COND_FALSE 0
    0@ == 0
then
    0AD1: show_formatted_text_highpriority "OR_FALSE=FAIL" time 2000
else
    0AD1: show_formatted_text_highpriority "OR_FALSE=PASS" time 2000
end

wait 2500

// ------------------------------------------------------------
// TEST 4: function returns TRUE, followed by FALSE => OR TRUE
// ------------------------------------------------------------

if or
    0AB1: cleo_call @COND_TRUE 0
    0@ == 0
then
    0AD1: show_formatted_text_highpriority "OR_TRUE=PASS" time 2000
else
    0AD1: show_formatted_text_highpriority "OR_TRUE=FAIL" time 2000
end

wait 2500

// ------------------------------------------------------------
// TEST 5: NOT over 0AB1 result
// ------------------------------------------------------------

if
    not 0AB1: cleo_call @COND_FALSE 0
then
    0AD1: show_formatted_text_highpriority "NOT_TRUE=PASS" time 2000
else
    0AD1: show_formatted_text_highpriority "NOT_TRUE=FAIL" time 2000
end

wait 3000

:LOOP
wait 1000
jump @LOOP


// ============================================================
// Function: returns TRUE as ConditionResult
// ============================================================

:COND_TRUE
if
    1 == 1
then
    0AB2: cleo_return 0
end

// ============================================================
// Function: returns FALSE as ConditionResult
// ============================================================

:COND_FALSE
if
    1 == 0
then
    0AB2: cleo_return 0
end

// The condition is false, but 0AB2 must still return from the
// function with that false ConditionResult.
0AB2: cleo_return 0
