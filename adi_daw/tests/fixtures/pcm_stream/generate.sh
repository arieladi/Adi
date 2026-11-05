#!/usr/bin/env bash
# Original synthetic signals, fixture generation only; no runtime CLI dependency.
set -euo pipefail
cd "$(dirname "$0")"
ffmpeg -hide_banner -loglevel error -f lavfi -i 'aevalsrc=0.2*sin(2*PI*431*t)|0.15*sin(2*PI*823*t):s=48000:d=20' -c:a libvorbis -q:a 6 -bitexact -map_metadata -1 -y long.ogg
ffmpeg -hide_banner -loglevel error -f lavfi -i 'aevalsrc=0.2*sin(2*PI*431*t)|0.15*sin(2*PI*823*t):s=48000:d=2' -c:a libmp3lame -b:a 192000 -write_xing 0 -bitexact -map_metadata -1 -y no-xing.mp3
ffmpeg -hide_banner -loglevel error -i ../decode/src16.wav -c:a flac -f ogg -bitexact -map_metadata -1 -y tone-flac.oga
