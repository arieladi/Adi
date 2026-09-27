#!/bin/sh
# Everything that can be checked without Max.
set -e
cd "$(dirname "$0")/.."
echo "=== regenerating patchers ==="
( cd build && python3 fft_analysis.py && python3 gen_patch.py && python3 capture.py && python3 device.py )
echo "=== structural validation ==="
python3 test/validate.py
echo "=== spectrum maths ==="
node test/test_spectrum.js
echo "=== weighting filters ==="
node test/test_weighting.js
