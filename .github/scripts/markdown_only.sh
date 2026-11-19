#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Does this pull request change anything a build leg compiles, runs or reads?
#
#   bash .github/scripts/markdown_only.sh >> "$GITHUB_OUTPUT"
#
# Run from the repository root, in a pull request's checkout fetched at depth 2.
# Prints one line, `code=true` or `code=false`, and says why on stderr.
#
# code=false means: every file the pull request changes is Markdown, and no
# build leg reads any of them. ci.yml then runs the validators and nothing
# else, and ui.yml skips the GUI shell. Before this, a pull request of notes or
# plans cost a full run, about 200 job-minutes from a pool of about 20 jobs.
#
# SOME MARKDOWN IS A BUILD INPUT, and a rule of "*.md never builds" would let a
# broken one through. check_spec_layout.py compares SPEC.md against the sizes
# the compiler produced, and adi_settings_tests reads OPS.md and
# SETTINGS-CATALOGUE.md. Those are found, not listed: a Markdown file whose
# name appears as a quoted string in the code the legs compile or the scripts
# they run is treated as code. A list here would be one more thing to forget
# the day a test starts reading another document.
#
# Every doubt answers code=true. Not a pull request, not GitHub's merge commit,
# a diff that cannot be read: all of it builds.
set -euo pipefail

say() { printf '%s\n' "$*" >&2; }

if [ "${GITHUB_EVENT_NAME:-}" != "pull_request" ]; then
  say "event ${GITHUB_EVENT_NAME:-unset}, not a pull request: every leg runs"
  echo "code=true"; exit 0
fi

# A pull request's checkout is GitHub's merge commit: first parent the base it
# merged into, second the pull request's head. HEAD^1..HEAD is then exactly
# what the pull request changes on its base, the same set as base...head.
if ! git rev-parse -q --verify 'HEAD^2' >/dev/null 2>&1 \
   || ! git rev-parse -q --verify 'HEAD^1' >/dev/null 2>&1; then
  say "HEAD is not the pull request merged into its base: every leg runs"
  echo "code=true"; exit 0
fi

# --no-renames: a rename shows its new name only, so a .cpp renamed to .md
# would otherwise look like Markdown alone.
if ! changed=$(git diff --no-renames --name-only 'HEAD^1' HEAD); then
  say "git diff failed: every leg runs"
  echo "code=true"; exit 0
fi
if [ -z "$changed" ]; then
  say "no changed files found: every leg runs"
  echo "code=true"; exit 0
fi

# Where a build leg can read a document from: what it compiles and what it runs.
# The validators are not here on purpose -- they read Markdown and always run.
readers=(adi_daw/src adi_daw/tests adi_daw/CMakeLists.txt adi_daw/cmake
         adi_daw/plugins adi_daw/pd .github/scripts)

code=false
while IFS= read -r f; do
  case "$f" in
    *.md)
      base=${f##*/}
      # git grep: 0 found, 1 not found, anything else an error -- which builds.
      rc=0
      git grep -q -F "\"$base\"" -- "${readers[@]}" || rc=$?
      if [ "$rc" -eq 1 ]; then
        say "markdown:    $f"
      elif [ "$rc" -eq 0 ]; then
        say "build input: $f (\"$base\" is read by a leg)"
        code=true
      else
        say "git grep failed on $f: every leg runs"
        code=true
      fi
      ;;
    *)
      say "code:        $f"
      code=true
      ;;
  esac
done <<< "$changed"

if [ "$code" = false ]; then
  say "only Markdown no leg reads: the validators run, the builds do not"
fi
echo "code=$code"
