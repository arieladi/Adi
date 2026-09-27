#!/bin/sh
# Copy the built device into the User Library.
#
# It REFUSES while Live is running, and that is the whole point of the script.
# Copying these files under a live device is what segfaulted Live on 2026-08-30:
# Max watches the files a patcher depends on, saw them change beneath a running
# patcher, and crashed inside jpatcher_filechanged trying to hot-reload.  The
# crash log is unambiguous and it cost a session to diagnose.
set -e
cd "$(dirname "$0")"

# pgrep -x lies about app bundles; match the executable path instead.
if pgrep -f "/Applications/Ableton Live.*/Contents/MacOS/Live" >/dev/null 2>&1; then
    echo "Ableton Live is running.  Quit Live first, then run this again."
    echo "(Copying under a running device crashes Max's file watcher.)"
    exit 1
fi

DST="$HOME/Music/Ableton/User Library/Presets/Audio Effects/Max Audio Effect"
[ -d "$DST" ] || { echo "not found: $DST"; exit 1; }

for f in "AVC Spectrum Meter.amxd" avc.fftanalysis.maxpat avc.capgonio.maxpat \
         avc.capscope.maxpat avc.specreduce.genjit avc.specui.js \
         avc.engine.js avc.meters.js; do
    cp "device/$f" "$DST/$f"
    printf '  %s\n' "$f"
done
echo "installed to $DST"
