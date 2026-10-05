{$CLEO .cs}
{$USE CLEO+}
{$USE newOpcodes}

0000: NOP
thread "AUDT25"

:MAIN
wait 1000

// --- LOAD + START (only when handle is 0) ---
if
    31@ == 0
then
    0AAC: 31@ = load_audiostream "cleo/audio/test.mp3"

    if
        31@ <> 0
    then
        // Configure stream
        2503: set_audio_stream_speed 31@ speed 1.0
        2504: set_audio_stream_volume_with_transition 31@ volume 1.0 time_ms 1000
        2505: set_audio_stream_speed_with_transition 31@ speed 1.0 time_ms 500
        250A: set_audio_stream_type 31@ type 2 // MUSIC

        // Repeat continuously
        250D: set_audio_stream_looping 31@ enable 1

        // PLAY
        0AAD: set_audiostream 31@ perform_action 1

        0AD1: show_formatted_text_highpriority "AUDIO: LOOP + PLAY" time 3000
    else
        0AD1: show_formatted_text_highpriority "AUDIO: LOAD FAILED" time 3000
    end
end

// --- POLL (handle valid AND stream is playing) ---
if and
    31@ <> 0
    2500: is_audio_stream_playing 31@
then
    2501: 2@ = get_audiostream_duration 31@
    2502: 3@ = get_audio_stream_speed 31@
    2507: 4@ = get_audio_stream_progress 31@
    2509: 5@ = get_audio_stream_type 31@
    250B: 6@ = get_audio_stream_progress_seconds 31@
end

jump @MAIN
