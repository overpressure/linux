#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# How a change moves up the layers (.github/workflows/layers.yml):
#
#   layers.sh FROM
#
# For every layer that takes its changes from FROM (.github/layers) and lacks some of FROM's commits,
# a pull request from FROM into it, unless one is open. Its checks build the headset's kernel where the
# layer is a headset's; it is merged with a merge commit, and its merge moves the change on.
set -euo pipefail
from=${1:?usage: layers.sh FROM}
repo=${GITHUB_REPOSITORY:-overpressure/linux}
layers=$(cd "$(dirname "$0")/.." && pwd)/layers
summary=${GITHUB_STEP_SUMMARY:-/dev/null}
say() { echo "$*" | tee -a "$summary" >&2; }

grep -v '^#' "$layers" | while read -r source into; do
	[ "$source" = "$from" ] || continue
	ahead=$(gh api "repos/$repo/compare/$into...$from" --jq .ahead_by)
	if [ "$ahead" = 0 ]; then
		say "$into has every commit of $from."
		continue
	fi
	open=$(gh pr list --repo "$repo" --head "$from" --base "$into" --state open --json number --jq '.[0].number // empty')
	if [ -n "$open" ]; then
		say "$from into $into: #$open is open ($ahead commits)."
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
	say "$from into $into: pull request opened ($ahead commits)."
done
