#!/usr/bin/env bash
# Clone or refresh the external code adi_daw reads and links against.
#
#   bash adi_daw/tools/fetch_external.sh              everything, ~930MB
#   bash adi_daw/tools/fetch_external.sh --build-only just what the build links
#   bash adi_daw/tools/fetch_external.sh --third-party-only
#
# Both target directories are gitignored: each clone is its own git repo, the
# same arrangement as VST-ADI/vital. Submodules are deliberately NOT fetched;
# none of the reference repos needs to build for us to read it.
#
# The licence boundary these two directories encode is not cosmetic. See
# docs/EXTERNAL-CODE.md before copying a single line out of reference/.
#
# ---------------------------------------------------------------------------
# third_party/ is PINNED; reference/ is not. This is deliberate (ADR-0024).
#
# third_party/ is compiled into our binaries and its headers decide our struct
# layouts, so "what did we build against" has to be answerable from a commit
# hash and nothing else. Every entry carries a tag AND the commit that tag
# pointed at when it was pinned, and the fetch is verified against that commit:
# a tag is a mutable ref, so matching the tag name alone proves nothing.
#
# reference/ is read for design and never compiled. Pinning it would defeat its
# purpose -- the point of having Ardour and Zrythm on disk is to see what they
# do NOW. Nothing in reference/ can affect a build, so nothing in reference/
# needs to be reproducible.
# ---------------------------------------------------------------------------

set -euo pipefail

cd "$(dirname "$0")/.."

WANT_REFERENCE=1
WANT_ALL_THIRD_PARTY=1
WANT_JUCE=0
case "${1:-}" in
    --build-only)        WANT_REFERENCE=0; WANT_ALL_THIRD_PARTY=0 ;;
    --with-juce)         WANT_REFERENCE=0; WANT_ALL_THIRD_PARTY=0; WANT_JUCE=1 ;;
    --third-party-only)  WANT_REFERENCE=0 ;;
    "")                  ;;
    *) echo "unknown option: $1" >&2
       echo "usage: $0 [--build-only | --with-juce | --third-party-only]" >&2; exit 2 ;;
esac

mkdir -p third_party
if [ "$WANT_REFERENCE" = 1 ]; then mkdir -p reference; fi

# repo                          dir         licence   tag       commit that tag pointed at            role
#
# `role` is build|later|juce.
#   build  the CMake tree links it today; CI fetches exactly these with
#          --build-only.
#   later  pinned and ready, not yet linked.
#   juce   fetched only by --with-juce. JUCE is ~800MB and ADI_WITH_JUCE is OFF
#          by default (ADR-0036 keeps adi_core and all ten suites buildable with
#          no JUCE present at all), so pulling it into every CI leg would cost
#          every ABI a large clone to build something none of them build.
# Moving a pin is a reviewed change, not a refresh: see ADR-0024.
THIRD_PARTY=(
  "SRombauts/SQLiteCpp          SQLiteCpp   MIT       3.3.3     59a047b8d3fe8574406ed73ab9fac0474e87bd03  build"
  "nlohmann/json                json        MIT       v3.12.0   55f93686c01528224f448c19128836e7df245f72  build"
  "bungee-audio-stretch/bungee  bungee      MPL-2.0   v2.4.30   8cb6977d0c1a1b411ac320493b3c7f5182ed2d22  later"
  "DNedic/lockfree              lockfree    MIT       3.0.1     ae6c4df124536218b0b1adfc21ab4921810a00a5  later"
  "free-audio/clap              clap        MIT       1.2.10    195b42a004144fab0b3cf95e9c067187d15365b7  build"
  "juce-framework/JUCE          JUCE        AGPL-3.0  9.0.2     72782788ce18c2d4d760b28e0921d6ffc6431102  juce"
)

# NOTE: helio is helio-fm/helio-sequencer, NOT Ahornberg/helio-workstation --
# that fork has been stale since January 2022.
# NOTE: zrythm is AGPL-3.0. Since ADR-0138 its code may be copied with
# attribution, making the copying project AGPLv3. See docs/EXTERNAL-CODE.md.
REFERENCE=(
  "Conceptual-Machines/magda-core         magda-core          GPL-3.0"
  "Tracktion/tracktion_engine             tracktion_engine    GPL-3.0-or-later/commercial"
  "Ardour/ardour                          ardour              GPL-2.0-or-later"
  "helio-fm/helio-sequencer               helio-sequencer     GPL-3.0"
  "zrythm/zrythm                          zrythm              AGPL-3.0  READ-ONLY"

  # --- DSP references for the plugin roadmap (ADR-0093) ---------------------
  # Each is here for ONE piece of maths; the comment says which. Two of these
  # replace what was asked for by name, because the name did not hold the maths:
  #   * lsp-plugins is a META-repo -- a 660 KB build index with no DSP in it at
  #     all. The limiter core, oversampler and true-peak meter live in
  #     lsp-dsp-units; the modes and lookahead logic in lsp-plugins-limiter.
  #   * ChowCentaur (jatinchowdhury18/KlonCentaur) contains NO ADAA. Its clipper
  #     is a wave-digital-filter diode pair. The ADAA maths is in the ADAA repo
  #     (the derivations) and chowdsp_utils (the production waveshapers). Both
  #     are fetched; KlonCentaur stays, for what it actually is.
  # ZLEqualizer is AGPL-3.0: the Zrythm rule applies. Read the design, implement
  # matched-phase from the published papers, copy nothing.
  "ZL-Audio/ZLEqualizer                   ZLEqualizer         AGPL-3.0  READ-ONLY"
  "lsp-plugins/lsp-dsp-units              lsp-dsp-units       LGPL-3.0-or-later"
  "lsp-plugins/lsp-plugins-limiter        lsp-plugins-limiter LGPL-3.0-or-later"
  "Sakhnovkrg/vitOTTx                     vitOTTx             GPL-3.0"
  "jatinchowdhury18/ADAA                  ADAA                BSD-3-Clause"
  "Chowdhury-DSP/chowdsp_utils            chowdsp_utils       per-module; waveshapers GPL-3.0"
  "jatinchowdhury18/KlonCentaur           KlonCentaur         BSD-3-Clause"
  "JDSherbert/Audio-Soft-Clip-Distortion  Audio-Soft-Clip-Distortion  MIT"
)

# --- pinned: third_party/ ---------------------------------------------------
fetch_pinned() {
    local repo dir lic tag want role path
    read -r repo dir lic tag want role <<<"$1"
    path="third_party/$dir"

    if [ "$role" = juce ] && [ "$WANT_JUCE" = 0 ] && [ "$WANT_ALL_THIRD_PARTY" = 0 ]; then
        printf '  %-18s skipped       (needs --with-juce)\n' "$dir"
        return 0
    fi
    if [ "$WANT_ALL_THIRD_PARTY" = 0 ] && [ "$role" != build ] && [ "$role" != juce ]; then
        printf '  %-18s skipped       (--build-only; role=%s)\n' "$dir" "$role"
        return 0
    fi

    # Checkout is always BY TAG, never by the pinned commit. Checking out the
    # pinned commit directly would force the tree to the right bytes and make
    # the assertion below tautological -- it would paper over a moved tag
    # instead of reporting it, which is the one thing this function exists for.
    # The commit is the assertion, not the source.
    if [ -d "$path/.git" ]; then
        printf '  %-18s checking...   ' "$dir"
        if [ "$(git -C "$path" rev-parse HEAD 2>/dev/null || echo none)" != "$want" ]; then
            git -C "$path" fetch --depth 1 -q --force origin \
                "refs/tags/$tag:refs/tags/$tag" 2>/dev/null || true
            git -C "$path" -c advice.detachedHead=false \
                checkout -q --detach "refs/tags/$tag" 2>/dev/null || true
        fi
    else
        printf '  %-18s cloning...    ' "$dir"
        git clone --depth 1 --branch "$tag" -c advice.detachedHead=false -q \
            "https://github.com/$repo.git" "$path" 2>/dev/null || true
    fi

    # A tag is a mutable ref. Matching the tag name proves nothing about the
    # bytes; only the commit does. If upstream re-points a tag, this is where
    # we find out -- loudly, and before anything is compiled.
    local got
    got="$(git -C "$path" rev-parse HEAD 2>/dev/null || echo unavailable)"
    if [ "$got" != "$want" ]; then
        printf 'MISMATCH\n'
        echo >&2
        echo "FATAL: $repo is not at its pinned commit." >&2
        echo "  pinned : $want   (tag $tag)" >&2
        echo "  fetched: $got" >&2
        echo >&2
        echo "Either upstream moved the tag, or someone edited this table without" >&2
        echo "updating the commit beside it. Do not 'fix' this by copying the new" >&2
        echo "hash in -- moving a pin is a reviewed change. See ADR-0024." >&2
        exit 1
    fi
    printf '%s %s  %-9s [%s]\n' "${got:0:9}" "$(git -C "$path" log -1 --format='%cs')" "$tag" "$lic"
}

# --- unpinned: reference/ ---------------------------------------------------
fetch_floating() {
    local repo dir lic path
    read -r repo dir lic <<<"$1"
    path="reference/$dir"

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

echo "third_party/ -- linked and shipped, PINNED (ADR-0024):"
for e in "${THIRD_PARTY[@]}"; do fetch_pinned "$e"; done

if [ "$WANT_REFERENCE" = 1 ]; then
    echo
    echo "reference/ -- read only, never on the include path, deliberately unpinned:"
    for e in "${REFERENCE[@]}"; do fetch_floating "$e"; done
fi

echo
echo "Sizes:"
du -sh third_party 2>/dev/null || true
if [ "$WANT_REFERENCE" = 1 ]; then du -sh reference 2>/dev/null || true; fi
echo "Licence boundary: docs/EXTERNAL-CODE.md"
