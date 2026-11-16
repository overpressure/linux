#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# How a change moves up the layers (.github/workflows/layers.yml):
#
#   layers.sh FROM
#
# For every layer that takes its changes from FROM (.github/layers) and lacks some of FROM's commits,
# a pull request from FROM into it, unless one is open. A pull request opened with the workflow's own
# token (GH_TOKEN) starts no workflow, so this starts its Build: a run on the layer with head=FROM, which
# builds every headset whose branch is the layer or built on it with FROM merged in and sets the status ci
# on FROM's commit (.github/workflows/build.yml). It is merged with a merge commit, and its merge moves the
# change on. An open pull request's Build starts again: FROM may have moved. GitHub also gives such a pull
# request a run of its own that waits for a maintainer's approval (its author, the workflow's bot, counts as a
# first-time contributor); that run would only build what the started one builds, so it is deleted.
set -euo pipefail
from=${1:?usage: layers.sh FROM}
repo=${GITHUB_REPOSITORY:-overpressure/linux}
layers=$(cd "$(dirname "$0")/.." && pwd)/layers
summary=${GITHUB_STEP_SUMMARY:-/dev/null}
say() { echo "$*" | tee -a "$summary" >&2; }

# The runs of FROM's pull requests that wait for approval (see the top): GitHub creates them a moment after
# the pull request opens or its head moves, so this looks for half a minute.
drop_waiting() {
	local query='.workflow_runs[] | select(.conclusion == "action_required" and .actor.login == "github-actions[bot]") | .id'
	for _ in 1 2 3 4 5 6; do
		sleep 5
		for id in $(gh api "repos/$repo/actions/runs?event=pull_request&branch=$1&per_page=20" --jq "$query"); do
			gh api -X DELETE "repos/$repo/actions/runs/$id" --silent && say "deleted run $id, which waited for approval"
		done
	done
}

grep -v '^#' "$layers" | while read -r source into; do
	[ "$source" = "$from" ] || continue
	ahead=$(gh api "repos/$repo/compare/$into...$from" --jq .ahead_by)
	if [ "$ahead" = 0 ]; then
		say "$into has every commit of $from."
		continue
	fi
	open=$(gh pr list --repo "$repo" --head "$from" --base "$into" --state open --json number --jq '.[0].number // empty')
	if [ -n "$open" ]; then
		gh workflow run build.yml --repo "$repo" --ref "$into" -f head="$from"
		say "$from into $into: #$open is open ($ahead commits); its Build started."
		drop_waiting "$from"
		continue
	fi
	head=$(gh api "repos/$repo/branches/$from" --jq .commit.sha)
	if [ "$from" = valve/7.2 ]; then
		title="Valve's kernel at ${head:0:12} into $into"
		intro="Valve's new commits of its public branch, into the first layer:"
	else
		title="$from into $into"
		intro="What $from has and $into lacks:"
	fi
	body=$(mktemp)
	{
		echo "$intro"
		echo
		echo '```'
		gh api "repos/$repo/compare/$into...$from" \
			--jq '.commits[] | "\(.sha[0:12]) \(.commit.author.name): \(.commit.message | split("\n")[0])"' | tail -n 250
		echo '```'
		echo
		echo "Merge it with a merge commit once its checks pass; then the layer built on $into gets its pull request."
		echo "A conflict is resolved here. Nothing is released until a headset's tag."
	} > "$body"
	gh pr create --repo "$repo" --head "$from" --base "$into" --title "$title" --body-file "$body" >/dev/null
	gh workflow run build.yml --repo "$repo" --ref "$into" -f head="$from"
	say "$from into $into: pull request opened ($ahead commits), its Build started."
	drop_waiting "$from"
done
