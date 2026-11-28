#!/usr/bin/env sh
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

if [ "$#" -ne 1 ]; then
    printf 'Usage: %s BUILD_DIR\n' "$0" >&2
    exit 2
fi

build_dir=$1
script_dir=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
scratch=$(mktemp -d)
trap 'rm -rf -- "$scratch"' EXIT
prefix="$scratch/install"
project="$scratch/Project with spaces.adi"

"$script_dir/install.sh" "$build_dir" "$prefix"
test -x "$prefix/bin/ADI"
test -x "$prefix/bin/adi_plugin_scanner"
test -s "$prefix/share/doc/adi/licenses/LICENSE"
test -s "$prefix/share/icons/hicolor/512x512/apps/org.adi.daw.png"
starter_drums="$prefix/share/adi/starter-drums"
test "$(find "$starter_drums/Hits" -maxdepth 1 -name '*.wav' -type f | wc -l)" -eq 12
test "$(find "$starter_drums/Loops" -maxdepth 1 -name '*.wav' -type f | wc -l)" -eq 4
desktop-file-validate "$prefix/share/applications/org.adi.daw.desktop"
grep -Fx 'application/x-adi-project=org.adi.daw.desktop;' "$prefix/share/applications/mimeinfo.cache"

"$build_dir/adi_ui_document_tests" --fixture "$project"
env XDG_DATA_HOME="$prefix/share" gio info --attributes=standard::content-type "$project" |
    grep -F 'standard::content-type: application/x-adi-project'
mv "$project" "$scratch/without-extension"
env XDG_DATA_HOME="$prefix/share" gio info --attributes=standard::content-type "$scratch/without-extension" |
    grep -F 'standard::content-type: application/x-adi-project'
