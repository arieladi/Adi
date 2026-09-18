#!/usr/bin/env bash
# Clone or refresh the external code adi_daw reads and links against.
#
#   bash adi_daw/tools/fetch_external.sh
#
# Both target directories are gitignored: each clone is its own git repo, the
# same arrangement as VST-ADI/vital. Clones are shallow -- we read this code, we
# do not track its history. Submodules are deliberately NOT fetched; none of the
# reference repos needs to build for us to read it.
#
# The licence boundary these two directories encode is not cosmetic. See
# docs/EXTERNAL-CODE.md before copying a single line out of reference/.

set -euo pipefail

cd "$(dirname "$0")/.."
mkdir -p third_party reference

# repo                                    dir                 licence
THIRD_PARTY=(
  "SRombauts/SQLiteCpp                    SQLiteCpp           MIT"
  "nlohmann/json                          json                MIT"
  "bungee-audio-stretch/bungee            bungee              MPL-2.0"
  "DNedic/lockfree                        lockfree            MIT"
)

# NOTE: helio is helio-fm/helio-sequencer, NOT Ahornberg/helio-workstation --
# that fork has been stale since January 2022.
# NOTE: zrythm is AGPL-3.0. Read it for design only; never copy code from it
# into a GPLv3 project. See docs/EXTERNAL-CODE.md.
REFERENCE=(
  "Conceptual-Machines/magda-core         magda-core          GPL-3.0"
  "Tracktion/tracktion_engine             tracktion_engine    GPL-3.0-or-later/commercial"
  "Ardour/ardour                          ardour              GPL-2.0-or-later"
  "helio-fm/helio-sequencer               helio-sequencer     GPL-3.0"
  "zrythm/zrythm                          zrythm              AGPL-3.0  READ-ONLY"
)

fetch() {
    local base="$1" repo dir lic
    read -r repo dir lic <<<"$2"
    local path="$base/$dir"

    if [ -d "$path/.git" ]; then
        printf '  %-18s refreshing... ' "$dir"
        git -C "$path" fetch --depth 1 origin -q
        git -C "$path" reset --hard -q "origin/$(git -C "$path" rev-parse --abbrev-ref origin/HEAD | sed 's|^origin/||')" 2>/dev/null \
            || git -C "$path" reset --hard -q FETCH_HEAD
    else
        printf '  %-18s cloning...    ' "$dir"
        git clone --depth 1 --single-branch -q "https://github.com/$repo.git" "$path"
    fi
    printf '%s  [%s]\n' "$(git -C "$path" log -1 --format='%h %cs')" "$lic"
}

echo "third_party/ -- linked and shipped:"
for e in "${THIRD_PARTY[@]}"; do fetch third_party "$e"; done

echo
echo "reference/ -- read only, never on the include path:"
for e in "${REFERENCE[@]}"; do fetch reference "$e"; done

echo
echo "total: $(du -sh third_party reference 2>/dev/null | awk '{s=$1} END {print s}') in reference/, see du -sh for detail"
echo "Licence boundary: docs/EXTERNAL-CODE.md"
