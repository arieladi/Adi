#!/usr/bin/env bash
# Synthetic, original test signals; no third-party recording. Fixture generation
# only: ffmpeg is never launched or required by ADI production/runtime/tests.
set -euo pipefail
cd "$(dirname "$0")"
ffmpeg -hide_banner -loglevel error -f lavfi -i 'aevalsrc=0.2*sin(2*PI*431*t):s=44100:d=0.25' -c:a aac -b:a 96000 -bitexact -map_metadata -1 -y mono.m4a
ffmpeg -hide_banner -loglevel error -f lavfi -i 'aevalsrc=0.2*sin(2*PI*431*t)|0.15*sin(2*PI*823*t):s=48000:d=0.25' -c:a aac -b:a 192000 -bitexact -map_metadata -1 -y stereo.m4a
for name in mono stereo; do
    ffmpeg -hide_banner -loglevel error -i "$name.m4a" -c:a copy -f adts -y "$name.aac"
    ffmpeg -hide_banner -loglevel error -i "$name.m4a" -c:a pcm_f32le -bitexact -map_metadata -1 -y "$name-reference.wav"
    ffmpeg -hide_banner -loglevel error -i "$name.aac" -c:a pcm_f32le -bitexact -map_metadata -1 -y "$name-adts-reference.wav"
done
