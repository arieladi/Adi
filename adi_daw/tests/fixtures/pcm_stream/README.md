# Synthetic PCM stream fixtures

Original generated signals, no third-party recording. `generate.sh` uses Debian
FFmpeg 7.1.5-0+deb13u1 only to generate the committed inputs. Production and tests
never invoke the CLI.

- `long.ogg`: 20-second stereo Vorbis at 48 kHz, 431 Hz / 0.2 on the left and
  823 Hz / 0.15 on the right. Its compressed size exceeds the normal C++ stream
  buffer, exposing the previous whole-file vector allocation. Only a real
  prefix is pulled in the streaming check, before the rest has decoded.
- `no-xing.mp3`: two seconds of the same authored stereo signal at 192 kbps,
  with the muxer's Xing/Info length header disabled. The pull stream must retain
  an unknown length until actual EOF rather than predecode to invent one.
  Signal amplitude/frequency is checked independently of codec delay; no trim
  is guessed. The complete cache and pull stream must still agree on PCM.
- `tone-flac.oga`: Ogg FLAC made independently from `../decode/src16.wav`;
  both the pull stream and ordinary cache are tested against original PCM.

SHA-256:

```
bc2e9f4bfc2da36d9eda57da05c0d9c17e5b8c3a17ebb714e48e72e1e521baa8  long.ogg
052e5c4180b6c7c1688fbcef113484d1fb4f40db3036dc6df43857608c0c36e9  no-xing.mp3
3647cb5f07eedb6ea4fcf79b324990e958785f2c2fb0a0e1d98f59b114cbee3f  tone-flac.oga
```

The older native decoder fixture set remains under `../decode`, including
independently generated lossless originals and encoder provenance.
