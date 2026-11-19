#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# markdown_only.sh's test table.
#
#   bash .github/scripts/test_markdown_only.sh
#
# Builds small throwaway repositories whose HEAD is shaped like GitHub's merge
# commit for a pull request (first parent the base, second the head), runs
# markdown_only.sh in each under the environment of one row, and compares its
# answer with the row's. ci.yml's `plan` runs this before it runs the script,
# so a rule that went wrong stops the run instead of quietly skipping legs.
# Needs only bash and git; no network and nothing from GitHub.
#
# THIS FILE MUST NOT QUOTE A MARKDOWN FILE'S NAME in double quotes. It lives in
# .github/scripts, one of the places markdown_only.sh searches for a quoted
# name, so a quoted name here would make that document a build input. The
# names below are built from variables for that reason.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
script="$here/markdown_only.sh"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# The test's own commits must not depend on whoever runs it: no signing, no
# hooks, no template from a user or system config.
export GIT_CONFIG_NOSYSTEM=1 GIT_CONFIG_GLOBAL=/dev/null
export GIT_AUTHOR_NAME=test GIT_AUTHOR_EMAIL=test@example.invalid
export GIT_COMMITTER_NAME=test GIT_COMMITTER_EMAIL=test@example.invalid

md=md                       # the extension, kept out of literal names
notes="docs/notes.$md"      # a document no build reads
input_doc="LAYOUT-SPEC.$md" # a document the compiled code names in quotes

# A repository with a base commit, then a pull request branch made by "$@"
# (a shell function run in the work tree), then the base merged with it the
# way GitHub merges a pull request: base first, head second.
make_repo() {
  local dir="$work/$1"; shift
  mkdir -p "$dir/adi_daw/src" "$dir/docs"
  (
    cd "$dir"
    git init -q
    # Not "head": on a case-insensitive file system that is HEAD.
    git checkout -q -b pr-base
    printf 'int a = 1;\n' > adi_daw/src/a.cpp
    printf 'int b = 1;\n' > adi_daw/src/b.cpp
    printf 'notes\n' > "$notes"
    printf 'layout\n' > "docs/$input_doc"
    # The compiled code reads one document by name, as a test reads SPEC.md.
    printf 'const char* kDoc = "%s";\n' "$input_doc" > adi_daw/src/reads_doc.cpp
    git add -A && git commit -q -m base
    git checkout -q -b pr-head
    "$@"
    git add -A && git commit -q -m head
    git checkout -q pr-base
    git merge -q --no-ff --no-edit pr-head
  )
}

change_notes()    { printf 'more notes\n' >> "$notes"; }
change_cpp()      { printf 'int a = 2;\n' > adi_daw/src/a.cpp; }
change_input()    { printf 'more layout\n' >> "docs/$input_doc"; }
rename_cpp_to_md() { git mv adi_daw/src/b.cpp "adi_daw/src/b.$md"; }

make_repo notes  change_notes
make_repo cpp    change_cpp
make_repo input  change_input
make_repo rename rename_cpp_to_md

# A HEAD that is not a merge commit at all: a push's checkout, or a pull
# request checked out at its head.
mkdir -p "$work/flat/docs"
(
  cd "$work/flat"
  git init -q
  printf 'notes\n' > "$notes"; git add -A; git commit -q -m one
  printf 'more\n' >> "$notes"; git add -A; git commit -q -m two
)

fails=0
rows=0
# row  LABEL  REPO  EVENT  PR_BASE_REF  GITHUB_BASE_REF  WANT
# An empty base is written '-' and means unset.
row() {
  local label=$1 repo=$2 event=$3 passed=$4 github=$5 want=$6 out
  local vars=("GITHUB_EVENT_NAME=$event")
  [ "$passed" = - ] || vars+=("PR_BASE_REF=$passed")
  [ "$github" = - ] || vars+=("GITHUB_BASE_REF=$github")
  rows=$((rows + 1))
  # env -u first: a base the caller's own environment carries (this runs inside
  # a pull request's CI job, where GitHub sets GITHUB_BASE_REF) must not leak in.
  out=$(cd "$work/$repo" && env -u PR_BASE_REF -u GITHUB_BASE_REF "${vars[@]}" \
          bash "$script" 2>/dev/null) || out="exit $?"
  if [ "$out" = "code=$want" ]; then
    printf '  ok    %s: %s\n' "$label" "$out"
  else
    printf '  FAIL  %s: got %s, wanted code=%s\n' "$label" "${out:-nothing}" "$want"
    fails=$((fails + 1))
  fi
}

row "a push to main builds"                          notes push         -        -        true
row "Markdown no build reads, into main"             notes pull_request main     main     false
row "C++ into main"                                  cpp   pull_request main     main     true
row "a document the code quotes, into main"          input pull_request main     main     true
row "a .cpp renamed to Markdown, into main"          rename pull_request main    main     true
row "HEAD is not a merge commit"                     flat  pull_request main     main     true
# The stack's top (review of 19 November): its base is the pull request below
# it, it lands with no new run, so Markdown alone must still build everything.
row "Markdown only, stacked on another branch"       notes pull_request window/x window/x true
row "stacked, only GitHub's base given"              notes pull_request -        window/x true
row "stacked, only the passed-in base given"         notes pull_request window/x -        true
row "into main, only GitHub's base given"            notes pull_request -        main     false
row "into main, only the passed-in base given"       notes pull_request main     -        false
row "the two bases disagree"                         notes pull_request window/x main     true
row "the two bases disagree, the other way"          notes pull_request main     window/x true
row "no base at all"                                 notes pull_request -        -        true
row "a branch whose name begins with main"           notes pull_request main-old main-old true
row "C++ stacked on another branch"                  cpp   pull_request window/x window/x true

printf '\n%s -- %d of %d row(s) wrong\n' "$([ $fails = 0 ] && echo PASS || echo FAILED)" "$fails" "$rows"
[ "$fails" = 0 ]
