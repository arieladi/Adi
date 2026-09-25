#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Fetch the third-party plug-ins ADI builds as CLAP (ADR-0166), pinned.
#
#   bash adi_daw/tools/fetch_plugins.sh [DEST]
#
# DEST defaults to adi_daw/third_party/plugins (gitignored). On Windows keep it
# SHORT -- JUCE's generated paths inside a build overrun MAX_PATH from a deep
# checkout -- e.g. C:/Users/<you>/Documents/GitHub/Adi-wt/plugins.
#
# Each entry is pinned by COMMIT, fetched at exactly that commit and checked,
# with its submodules at the commits it records (ADR-0024: moving a pin is a
# reviewed change). None of this is compiled into the DAW: each plug-in is its
# own binary under its own licence, built by plugins/external/<name> or, for
# ChowTape, by its own tree. See plugins/README.md for what each becomes.

set -euo pipefail

cd "$(dirname "$0")/.."
DEST="${1:-third_party/plugins}"
mkdir -p "$DEST"

# dir               repo                                              commit                                    date        licence       submodules
PLUGINS=(
  "smartelectronix  https://github.com/bdejong/smartelectronix.git     248d2c4044930d732766b522573531edc3656300  2026-04-15  GPL-3.0       no"
  "AnalogTapeModel  https://github.com/jatinchowdhury18/AnalogTapeModel.git 604372e4ffd9690c3e283362e4598cb43edbb475 2023-11-05 GPL-3.0 yes"
  "KlonCentaur      https://github.com/jatinchowdhury18/KlonCentaur.git f3bb633a593b6fbb22a44c1ef9d1dbedbfe92d5b  2021-07-02  BSD-3-Clause  yes"
  "ZLEqualizer      https://github.com/ZL-Audio/ZLEqualizer.git        3468a3ac85f5c1f9d16083acbee5b1339984d53b  2026-09-22  AGPL-3.0      yes"
  "dragonfly-reverb https://github.com/michaelwillis/dragonfly-reverb.git 440ec7b3b3db2fec34b9f80cf4890102ca14d1c3 2026-05-21 GPL-3.0     yes"
  "airwin2rack      https://github.com/baconpaul/airwin2rack.git       b6eef0af60cd32641b09837096e41bbcdb030341  2026-09-19  MIT           no"
)

for row in "${PLUGINS[@]}"; do
    read -r dir repo commit date licence submodules <<<"$row"
    path="$DEST/$dir"
    if [ ! -d "$path/.git" ]; then
        git init -q "$path"
        git -C "$path" remote add origin "$repo"
    fi
    if [ "$(git -C "$path" rev-parse HEAD 2>/dev/null || echo none)" != "$commit" ]; then
        git -C "$path" fetch -q --depth 1 origin "$commit"
        git -C "$path" -c advice.detachedHead=false checkout -q --force FETCH_HEAD
    fi
    if [ "$submodules" = yes ]; then
        git -C "$path" submodule update -q --init --recursive
    fi
    got="$(git -C "$path" rev-parse HEAD)"
    if [ "$got" != "$commit" ]; then
        echo "$dir: at $got, pinned to $commit" >&2
        exit 1
    fi
    printf '%-17s %s  %s  [%s]\n' "$dir" "${got:0:10}" "$date" "$licence"
done

echo
echo "Sources in $DEST. Build: tools/build-external-plugin.bat <name> (Windows),"
echo "and ADI Airwindows with ADI_AIRWIN_SOURCE=$DEST/airwin2rack tools/build-plugins.bat."
