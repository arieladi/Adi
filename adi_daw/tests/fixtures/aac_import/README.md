# Synthetic independent AAC import fixtures

These original signals contain no third-party recording. `generate.sh` made them
with Debian FFmpeg **7.1.5-0+deb13u1**, independently of ADI's encoder, MP4 parser,
ADTS parser and cache. FFmpeg is a fixture-generation tool only; it is never
launched or required by the production program or this test suite.

- `mono.m4a`: AAC-LC, 44.1 kHz, mono, 11,025 authored samples, 1,024 priming samples.
- `stereo.m4a`: AAC-LC, 48 kHz, stereo, 12,000 authored samples, 1,024 priming samples.
  The two channels have different frequencies and amplitudes.
- `*.aac`: the same compressed packets remuxed into ADTS. This transport has no
  authored priming/tail duration; every coded 1,024-sample access unit is retained.
- `*-reference.wav`: FFmpeg's independent float32 decode. Its MP4 output may
  include final coded padding beyond the authored end; the test checks the
  authored frame count separately and compares that exact prefix. ADTS reference
  dimensions and all samples are checked without inferred trimming.

Compressed fixture SHA-256:

```
cd118fb47f127ddeae1620eebe769833e4694ad05f2a51e1e8ad488342411e9d  mono.m4a
006051c8ee9ac522c8196955dcc3a09ee88da4b77b82cfdc9c54ba807ea5704b  stereo.m4a
b91048d2f0269603d32fcc91e861d7a89b32c5c632830890f2d9ecabda737f5f  mono.aac
fbfb014a02a192382a2970a8019978d4e1c25e7e715ceec566ed9eba58136bc0  stereo.aac
```

The numerical comparison allows small differences between native decoder
implementations. It is a check of these fixtures, not a claim of parity for every
AAC profile, container or operating system. Existing NI Stems tests continue to
exercise ADI's encoder and the stricter aligned-stereo transport.
