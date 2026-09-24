# Decoder fixtures (ADR-0156)

Small, generated, committed. `make_fixtures.py` here regenerates every file;
nothing was downloaded, recorded or copied from anywhere. The PCM is computed by
the script: sine chords (440 Hz on the left, 660 Hz on the right, 1 kHz on both)
plus low-level deterministic noise from an LCG, so FLAC has real work to do.

Encoders: **flac 1.4.3, LAME 3.100, oggenc (vorbis-tools) 1.4.2**, from Ubuntu's
packages, run on 2026-09-24.

| File | Made by | What the test proves with it |
|---|---|---|
| `src16.wav` | the script: 16-bit stereo, 48 kHz, 9,600 frames | the reference PCM for the FLAC, AIFF, MP3 and Vorbis files made from it |
| `src24.wav` | the script: 24-bit mono, 44.1 kHz, 4,410 frames | the reference for `tone24.flac` |
| `tone16.flac` | `flac -8 --no-padding src16.wav` | decodes bit-exact to `src16.wav` |
| `tone24.flac` | `flac -8 --no-padding src24.wav` | decodes bit-exact to `src24.wav` |
| `rate768.flac` | the script's 24-bit mono 768 kHz PCM, 7,680 frames, `flac -8 --lax` (above FLAC's subset rates); the WAV is not kept | ADR-0157's top rate decodes, at that rate |
| `rate8k.flac` | the script's 16-bit mono 8 kHz PCM, 1,600 frames, `flac -8`; the WAV is not kept | a low rate is kept; the clip path converts |
| `tone.aiff` | the script: `src16.wav`'s samples, big-endian, in AIFF | AIFF big-endian decodes bit-exact |
| `tone.aifc` | the script: the same samples, AIFC `sowt` (little-endian) | AIFC decodes bit-exact |
| `u8.wav` | the script: the top byte of `src16.wav`'s left channel, first 2,400 frames, 8-bit unsigned | a WAV the WavReader refuses is decoded |
| `tone.mp3` | `lame -b 192 --noreplaygain src16.wav` | MP3 above 25 dB SNR (measured 30.3), gapless: 9,600 frames at lag 0 |
| `tone.ogg` | `oggenc -q 6 --serial 1 src16.wav` | Vorbis above 32 dB SNR (measured 37.7), exact length |
| `truncated.flac` | the first 3/5 of `tone16.flac`'s bytes | refused as `decode.corrupt` |
| `not_audio.bin` | a repeated sentence | refused as `decode.unknown_format` |

Other encoder versions change the compressed bytes. The tests allow for that:
FLAC must still decode bit-exact, and MP3 and Vorbis must stay above their SNR
floors, each about 5 dB under the measured value.
