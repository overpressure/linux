#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Valve's kernel, followed (.github/workflows/valve.yml): valve/7.2 moved forward to the newest commit of
# Valve's public branch (through the evlaV mirror), then the Layers workflow started for it, which opens
# the pull request into the first layer. valve/7.2 is Valve's history as it is: when Valve rewrote its
# branch, nothing moves and the run fails.
set -euo pipefail
MIRROR=${MIRROR:-https://github.com/evlaV/deckard-linux-no-oopsies.git}
MIRROR_BRANCH=${MIRROR_BRANCH:-sm8650-7.2}
BASE=valve/7.2
repo=${GITHUB_REPOSITORY:-overpressure/linux}
summary=${GITHUB_STEP_SUMMARY:-/dev/null}
say() { echo "$*" | tee -a "$summary" >&2; }

new=$(git ls-remote "$MIRROR" "refs/heads/$MIRROR_BRANCH" | cut -f1)
old=$(git ls-remote origin "refs/heads/$BASE" | cut -f1)
if [ -z "$new" ] || [ -z "$old" ]; then
	say "could not read Valve's branch or $BASE"
	exit 1
fi
if [ "$new" = "$old" ]; then
	say "Valve's $MIRROR_BRANCH is ${old:0:12}, as $BASE."
else
	# Valve's new commits, deep enough to reach the old head: the push sends only what the repository lacks.
	git fetch -q --depth=1 origin "+refs/heads/$BASE:refs/remotes/origin/$BASE"
	git fetch -q --depth=50 "$MIRROR" "$new"
	tries=0
	until git merge-base --is-ancestor "$old" "$new" 2>/dev/null; do
		tries=$((tries + 1))
		if [ "$tries" -gt 40 ]; then
			say "**Valve's ${new:0:12} does not descend from $BASE (${old:0:12}) within 2000 commits:** Valve rewrote its branch. Nothing moved; this needs a maintainer."
			exit 1
		fi
		git fetch -q --deepen=50 "$MIRROR" "$new"
	done
	git push -q origin "$new:refs/heads/$BASE"
	say "$BASE moved from ${old:0:12} to ${new:0:12} ($(git rev-list --count "$old..$new") commits)."
fi
# A push by the workflow's token starts no workflow: the Layers workflow is started by hand, from the
# default branch (valve/7.2's own tree has no workflows).
gh workflow run layers.yml --repo "$repo" --ref "${GITHUB_REF_NAME:-phoenix/7.2}" -f from="$BASE"
