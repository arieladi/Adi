#!/usr/bin/env bash
# Clone (or refresh) the Surge XT fork and its submodules for adi-surge.
#
#   bash adi-surge/tools/fetch_surge.sh            clone if missing, then init submodules
#   bash adi-surge/tools/fetch_surge.sh --full     same, but full submodule history
#
# adi-surge/surge/ is ITS OWN git repository, gitignored by the monorepo, exactly
# the arrangement adi-vst/vital uses (adi-vst ADR-0001). It carries an `upstream`
# remote pointing at surge-synthesizer/surge so that
#
#     git -C adi-surge/surge diff upstream/main --stat
#
# always shows our entire fork delta. Do not `git add` it into the monorepo: that
# would create a stray gitlink, which is the thing the ignore rule prevents.
#
# ---------------------------------------------------------------------------
# TWO TRAPS THIS SCRIPT EXISTS TO ABSORB. Both cost real time when hit raw.
#
# 1. `git submodule update --init --recursive` FAILS on a fresh Surge clone with
#
#        fatal: transport 'file' not allowed
#
#    even though every URL in .gitmodules is https. Git >= 2.38 refuses the local
#    `file://` transport by default (CVE-2022-39253) and git's submodule clone
#    path tries the superproject's object store first. The fix is to allow the
#    file transport for THIS command only -- which is what the -c flag below
#    does. Do not set protocol.file.allow=always globally; it is a global
#    weakening of a real mitigation for a problem that is local to this clone.
#
# 2. `.gitmodules` declares 23 submodules; only 22 are gitlinks in the tree.
#    `src/surge-rs/surge-rs` (the Rust bindings) is a stale declaration with no
#    gitlink behind it, so git silently never fetches it and `git submodule
#    status` never mentions it. That is upstream's state, not a broken checkout.
#    Nothing in the CMake build references it. Do not "fix" it.
# ---------------------------------------------------------------------------

set -euo pipefail

cd "$(dirname "$0")/.."          # -> adi-surge/

UPSTREAM_URL="https://github.com/surge-synthesizer/surge.git"
DEPTH=(--depth 1)
case "${1:-}" in
    --full) DEPTH=() ;;
    "")     ;;
    *) echo "usage: $0 [--full]" >&2; exit 2 ;;
esac

# --- the fork itself --------------------------------------------------------
if [ ! -d surge/.git ]; then
    echo "==> cloning Surge XT into adi-surge/surge (remote named 'upstream')"
    # Full history, deliberately: `git diff upstream/main` is the whole point of
    # the fork-not-vendor arrangement and a shallow clone cannot answer it.
    git clone --origin upstream "$UPSTREAM_URL" surge
else
    echo "==> adi-surge/surge already present; fetching upstream"
    git -C surge remote get-url upstream >/dev/null 2>&1 \
        || git -C surge remote add upstream "$UPSTREAM_URL"
    git -C surge fetch upstream
fi

echo "==> surge HEAD: $(git -C surge rev-parse --short HEAD)  $(git -C surge log -1 --format=%s)"

# --- submodules -------------------------------------------------------------
# The pins are the gitlinks in Surge's own tree, so there is nothing for us to
# record or verify separately: git checks out exactly the commit the superproject
# names, and `git submodule status` prefixes any drift with '+'. This is the one
# place adi-surge is SIMPLER than adi_daw, whose third_party/ pins are ours to
# assert (adi_daw ADR-0024) because they are independent clones, not gitlinks.
echo "==> initialising submodules (protocol.file.allow -- see trap 1 above)"
git -C surge -c protocol.file.allow=always \
    submodule update --init --recursive "${DEPTH[@]}" --jobs 4

echo
echo "==> submodule status (a leading '+' means drift from the pinned commit):"
git -C surge submodule status --recursive | sed 's/^/    /'

echo
echo "==> done. Build instructions: adi-surge/ARCHITECTURE.md section 2."
