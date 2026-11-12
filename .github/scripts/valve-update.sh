#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Valve's new kernel as a proposal, and landing it (.github/workflows/valve.yml).
#
#   valve-update.sh propose    the layers rebased onto Valve's newest commit, on <layer>-next branches,
#                              proposed as a pull request per headset, each headset's branch built
#   valve-update.sh land       the <layer>-next branches become the layers, valve/7.2 moves, a tag
#                              valve/7.2-<commit> marks Valve's commit; refused unless every headset's
#                              -next branch built
#
# Valve's commits come from the evlaV mirror of its public branch (MIRROR, MIRROR_BRANCH). The
# layers are LAYERS, each on the one before it, the first on valve/7.2. A rebase that conflicts
# pushes nothing and fails the run, naming the commit. Nothing here releases a kernel: a release is
# a tag that a maintainer pushes.
set -euo pipefail

MIRROR=${MIRROR:-https://github.com/evlaV/deckard-linux-no-oopsies.git}
MIRROR_BRANCH=${MIRROR_BRANCH:-sm8650-7.2}
BASE=valve/7.2
LAYERS=(common/7.2 sm8250/7.2 phoenix/7.2)
#: The layers that are a headset's (the others build nothing): they get a pull request and a build.
HEADSETS=(phoenix/7.2)
summary=${GITHUB_STEP_SUMMARY:-/dev/null}
repo=${GITHUB_REPOSITORY:-overpressure/linux}

say() { echo "$*" | tee -a "$summary" >&2; }
remote_sha() { git ls-remote origin "refs/heads/$1" | cut -f1; }

propose() {
	local new old existing
	new=$(git ls-remote "$MIRROR" "refs/heads/$MIRROR_BRANCH" | cut -f1)
	old=$(remote_sha "$BASE")
	if [ -z "$new" ] || [ -z "$old" ]; then
		say "could not read Valve's or our base"
		exit 1
	fi
	if [ "$new" = "$old" ]; then
		say "Valve's $MIRROR_BRANCH is ${old:0:12}, our $BASE: nothing new."
		return 0
	fi
	existing=$(remote_sha "$BASE-next")
	if [ "$existing" = "$new" ]; then
		say "Valve's ${new:0:12} is proposed already ($BASE-next)."
		return 0
	fi

	# The layers with their commits down to the old base: the checkout is shallow, and a fetch with
	# --depth does not deepen a tip that is there already, so each layer is deepened until the old
	# base is its ancestor. Then Valve's new head, deep enough to reach the old base: a push needs
	# history that the repository already has.
	local refspecs tries=0 layer
	refspecs=("+refs/heads/$BASE:refs/remotes/origin/$BASE")
	for layer in "${LAYERS[@]}"; do refspecs+=("+refs/heads/$layer:refs/remotes/origin/$layer"); done
	git fetch -q --depth=100 origin "${refspecs[@]}"
	for layer in "${LAYERS[@]}"; do
		until git merge-base --is-ancestor "$old" "refs/remotes/origin/$layer" 2>/dev/null; do
			tries=$((tries + 1))
			[ "$tries" -le 20 ] || { say "$layer does not reach $BASE within 2000 commits"; exit 1; }
			git fetch -q --deepen=100 origin "${refspecs[@]}"
		done
	done
	git fetch -q --depth=50 "$MIRROR" "$new"
	tries=0
	until git merge-base --is-ancestor "$old" "$new" 2>/dev/null; do
		tries=$((tries + 1))
		[ "$tries" -le 40 ] || { say "Valve's ${new:0:12} does not descend from our ${old:0:12} within 2000 commits: Valve rewrote its branch; rebase by hand"; exit 1; }
		git fetch -q --deepen=50 "$MIRROR" "$new"
	done

	local prev_old=$old prev_new=$new
	for layer in "${LAYERS[@]}"; do
		git checkout -q -B "$layer-next" "refs/remotes/origin/$layer"
		if ! git rebase -q --onto "$prev_new" "$prev_old" "$layer-next" >/dev/null 2>&1; then
			local commit subject files
			commit=$(git rev-parse --short=12 REBASE_HEAD)
			subject=$(git log -1 --format=%s REBASE_HEAD)
			files=$(git diff --name-only --diff-filter=U | tr '\n' ' ')
			git rebase --abort
			say "**$layer does not rebase onto Valve's ${new:0:12}:** $commit \"$subject\" conflicts in $files. Nothing was pushed; rebase by hand."
			exit 1
		fi
		prev_old=$(git rev-parse "refs/remotes/origin/$layer")
		prev_new=$(git rev-parse HEAD)
	done

	local pushes=("+$new:refs/heads/$BASE-next")
	for layer in "${LAYERS[@]}"; do pushes+=("+refs/heads/$layer-next:refs/heads/$layer-next"); done
	git push -q origin "${pushes[@]}"
	say "Valve's ${new:0:12} proposed: $BASE-next and ${LAYERS[*]/%/-next}."

	local top=${LAYERS[-1]} body
	body=$(mktemp)
	{
		echo "Valve's \`$MIRROR_BRANCH\` moved from \`${old:0:12}\` to \`${new:0:12}\`, and every layer rebased onto it without a conflict."
		echo
		echo "Valve's new commits:"
		echo
		echo '```'
		git log --format='%h %an: %s' "$old..$new"
		echo '```'
		echo
		echo "<details><summary>Our commits before and after (git range-diff; = is unchanged)</summary>"
		echo
		echo '```'
		git range-diff "$old..refs/remotes/origin/$top" "$new..$top-next" | cut -c1-160 | head -400
		echo '```'
		echo "</details>"
		echo
		echo "This pull request is for review: its CI builds the headset's branch. **Do not merge it with the button.** Land it with the Valve workflow's \`land\` action, which moves every layer and \`$BASE\` and tags Valve's commit; a release is a new tag afterwards."
	} > "$body"

	if [ -n "${GH_TOKEN:-}" ]; then
		local headset title
		for headset in "${HEADSETS[@]}"; do
			title="Valve's kernel at ${new:0:12}: $headset rebased"
			if [ -n "$(gh pr list --repo "$repo" --head "$headset-next" --base "$headset" --json number --jq '.[].number')" ]; then
				gh pr edit "$headset-next" --repo "$repo" --title "$title" --body-file "$body" >/dev/null
			else
				gh pr create --repo "$repo" --head "$headset-next" --base "$headset" --title "$title" --body-file "$body" >/dev/null
			fi
			# A push by the workflow's token starts no workflow: the build is started by hand.
			gh workflow run build.yml --repo "$repo" --ref "$headset-next"
			say "$headset-next: pull request and build started."
		done
	else
		cat "$body" >&2
	fi
}

land() {
	local new layer headset run
	new=$(remote_sha "$BASE-next")
	[ -n "$new" ] || { say "nothing proposed ($BASE-next is missing)"; exit 1; }
	for headset in "${HEADSETS[@]}"; do
		run=$(gh run list --repo "$repo" --workflow build.yml --branch "$headset-next" --limit 1 \
			--json conclusion,headSha --jq '.[0] | "\(.conclusion) \(.headSha)"')
		[ "$run" = "success $(remote_sha "$headset-next")" ] ||
			{ say "$headset-next has no successful build of its current commit ($run): not landed"; exit 1; }
	done
	# Valve's commit keeps its tag whatever the mirror does later.
	local tag
	tag=$(gh api "repos/$repo/git/tags" -f tag="$BASE-${new:0:12}" -f object="$new" -f type=commit \
		-f message="Valve's linux-valve at ${new:0:12}, from $MIRROR_BRANCH" --jq .sha)
	gh api "repos/$repo/git/refs" -f ref="refs/tags/$BASE-${new:0:12}" -f sha="$tag" >/dev/null
	for layer in "$BASE" "${LAYERS[@]}"; do
		gh api -X PATCH "repos/$repo/git/refs/heads/$layer" -f sha="$(remote_sha "$layer-next")" -F force=true >/dev/null
	done
	for headset in "${HEADSETS[@]}"; do
		gh pr close "$headset-next" --repo "$repo" --comment "Landed: the layers and $BASE are at Valve's ${new:0:12}." >/dev/null || true
	done
	for layer in "$BASE" "${LAYERS[@]}"; do
		gh api -X DELETE "repos/$repo/git/refs/heads/$layer-next" >/dev/null
	done
	say "Landed Valve's ${new:0:12}: $BASE, ${LAYERS[*]} moved; tag $BASE-${new:0:12}."
}

case ${1:-} in
propose) propose ;;
land) land ;;
*)
	sed -n '3,14p' "$0" >&2
	exit 2
	;;
esac
