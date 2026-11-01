#!/bin/bash
# ci-runs.sh <sha> <workflow>... : print one line per workflow, "<name> <conclusion> <url>",
# for the newest run of that workflow on <sha> in the fork repository. Exits 1 if
# the commit is not on the fork, or if any named workflow has no completed,
# successful run on it. Used by the build scripts in this folder.
set -euo pipefail
REPO=dave/maplibre-native
SHA=${1:?usage: ci-runs.sh <sha> <workflow>...}
shift
[ "$#" -gt 0 ] || { echo "ci-runs: name at least one workflow" >&2; exit 2; }
if ! gh api "repos/$REPO/commits/$SHA" --silent 2>/dev/null; then
  echo "ci-runs: $SHA is not on github.com/$REPO: push it and run CI first" >&2
  exit 1
fi
RUNS=$(gh run list --repo "$REPO" --commit "$SHA" --limit 200 \
  --json databaseId,workflowName,status,conclusion,url,createdAt)
fail=0
for wf in "$@"; do
  line=$(jq -r --arg wf "$wf" \
    '[.[] | select(.workflowName == $wf)] | sort_by(.createdAt) | last
     | if . == null then "" else "\(.status) \(.conclusion) \(.url)" end' <<<"$RUNS")
  if [ -z "$line" ]; then
    echo "ci-runs: no $wf run on $SHA" >&2; fail=1; continue
  fi
  read -r status conclusion url <<<"$line"
  if [ "$status" != completed ] || [ "$conclusion" != success ]; then
    echo "ci-runs: the newest $wf run on $SHA is $status/$conclusion: $url" >&2; fail=1; continue
  fi
  echo "$wf $conclusion $url"
done
exit $fail
