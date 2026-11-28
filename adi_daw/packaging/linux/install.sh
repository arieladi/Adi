#!/usr/bin/env sh
# SPDX-License-Identifier: AGPL-3.0-or-later
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    printf 'Usage: %s BUILD_DIR [PREFIX]\n' "$0" >&2
    exit 2
fi

build_dir=$1
prefix=${2:-"$HOME/.local"}
for command in cmake update-mime-database update-desktop-database; do
    if ! command -v "$command" >/dev/null 2>&1; then
        printf 'Required installer command is missing: %s\n' "$command" >&2
        exit 1
    fi
done

cmake --install "$build_dir" --config "${ADI_INSTALL_CONFIG:-Release}" --prefix "$prefix" --component adi_base
update-mime-database "$prefix/share/mime"
update-desktop-database "$prefix/share/applications"
