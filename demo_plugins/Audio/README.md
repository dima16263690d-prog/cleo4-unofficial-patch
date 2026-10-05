# Audio.cleo — extended audio for CLEO 4

Audio.cleo adds the modern CLEO 5-style 25xx audio controls while the original CLEO 4 audio opcodes remain in the core runtime.

## Legacy layer

`0AAC` `0AAD` `0AAE` `0AAF` `0AB9` `0ABB` `0ABC` `0AC0` `0AC1` `0AC2` `0AC3` `0AC4` `0AC5` remain the legacy stream API.

## Extended layer

`2500` playing check  
`2501` duration  
`2502` speed read  
`2503` speed set  
`2504` volume transition  
`2505` speed transition  
`2506` 3D source size  
`2507` normalized progress read  
`2508` normalized progress set  
`2509` stream type read  
`250A` stream type set  
`250B` progress in seconds  
`250C` progress in seconds set

## Stream types

`0` = None, `1` = SoundEffect, `2` = Music, `3` = UserInterface.

## Runtime design

The plugin does not initialize its own BASS device and does not duplicate stream ownership. It calls the CLEO 4 core Audio API, so old and new commands operate on the same stream handles.

## Test

`tests/AUDIO_25XX_TEST.cs` loads a stream with legacy `0AAC`, then exercises the new `2500–250C` plugin commands on the same handle.
