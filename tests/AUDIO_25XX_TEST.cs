{$CLEO .cs}
thread "AUDT25"

:MAIN
wait 1000

if
    31@ == 0
then
    // Audio files are stored in GTA San Andreas\CLEO\audio\
    0AAC: 31@ = load_audiostream "cleo/audio/test.mp3"
    if
        31@ <> 0
    then
        2503: set_audio_stream_speed 31@ speed 1.0
        2504: set_audio_stream_volume_with_transition 31@ volume 1.0 time_ms 1000
        2505: set_audio_stream_speed_with_transition 31@ speed 1.0 time_ms 500
        250A: set_audio_stream_type 31@ type 2
        1@ = 1
    end
end

if
    1@ == 1
then
    2500: is_audio_stream_playing 31@
    if
    then
        2507: 2@ = get_audio_stream_progress 31@
        250B: 3@ = get_audio_stream_progress_seconds 31@
        2502: 4@ = get_audio_stream_speed 31@
        2509: 5@ = get_audio_stream_type 31@
    end
end

jump @MAIN
