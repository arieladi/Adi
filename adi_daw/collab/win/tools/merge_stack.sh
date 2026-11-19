#!/bin/sh
# usage: merge_stack.sh N N N ...   merges PRs in order onto main: ready, retarget to main, wait for mergeability, merge
R=arieladi/Adi
for n in "$@"; do
  d=$(gh pr view $n --repo $R --json isDraft,state --jq '"\(.isDraft) \(.state)"')
  case "$d" in *MERGED*) echo "$n already merged"; continue;; esac
  case "$d" in true*) gh pr ready $n --repo $R >/dev/null 2>&1 && echo "$n ready";; esac
  b=$(gh pr view $n --repo $R --json baseRefName --jq .baseRefName)
  if [ "$b" != "main" ]; then gh pr edit $n --repo $R --base main >/dev/null 2>&1 && echo "$n base -> main (was $b)"; fi
  i=0; m=UNKNOWN
  while [ $i -lt 24 ]; do
    m=$(gh pr view $n --repo $R --json mergeable --jq .mergeable)
    [ "$m" != "UNKNOWN" ] && break
    sleep 5; i=$((i+1))
  done
  if [ "$m" != "MERGEABLE" ]; then echo "STOP: $n is $m"; exit 1; fi
  if gh pr merge $n --repo $R --merge --delete-branch=false >/dev/null 2>&1; then
    s=$(gh pr view $n --repo $R --json state --jq .state); echo "$n merged ($s)"
    [ "$s" = "MERGED" ] || { echo "STOP: $n state $s"; exit 1; }
  else echo "STOP: merge of $n failed"; exit 1; fi
done
echo "ALL DONE"
