#!/bin/sh
# Poll for the window batch's pull requests (branches window/<topic>, not window/log) and land each
# in number order as its checks pass, for up to HOURS hours. Already-landed numbers are skipped.
R=arieladi/Adi
SP="$(cd "$(dirname "$0")" && pwd)"
HOURS=${HOURS:-6}
end=$(( $(date +%s) + HOURS*3600 ))
while [ "$(date +%s)" -lt "$end" ]; do
  n=$(gh pr list --repo $R --state open --json number,headRefName --jq '[.[] | select((.headRefName|startswith("window/")) and .headRefName!="window/log") | .number] | sort | .[0] // empty')
  if [ -n "$n" ]; then
    echo "$(date +%H:%M) landing #$n"
    sh "$SP/land_top.sh" "$n" || echo "$(date +%H:%M) #$n did not land (see above); continuing"
    sleep 60
  else
    sleep 300
  fi
done
echo "POLL END"
