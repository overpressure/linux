#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Enough history in a shallow checkout (.github/workflows/build.yml) to merge or list commits:
#
#   history.sh [--merge] BRANCH COMMIT
#
# Fetches BRANCH (as origin/BRANCH) and COMMIT, deeper each round, until their merge base is known and,
# without --merge, every commit between it and COMMIT has its parents (git format-patch needs them);
# then prints the merge base. The kernel's whole history is a few GB; the layers' own commits are a few
# hundred.
set -euo pipefail
merge=false
if [ "${1:-}" = --merge ]; then
	merge=true
	shift
fi
branch=${1:?usage: history.sh [--merge] BRANCH COMMIT}
commit=${2:?usage: history.sh [--merge] BRANCH COMMIT}
want=("+refs/heads/$branch:refs/remotes/origin/$branch" "$commit")
shallow=$(git rev-parse --git-path shallow)

# Whether every commit from BASE to COMMIT has its parents here.
whole() {
	$merge || [ ! -s "$shallow" ] || ! git rev-list "$1..$commit" | grep -qxFf "$shallow"
}

git fetch -q --no-tags --depth=50 origin "${want[@]}"
for _ in $(seq 30); do
	if base=$(git merge-base "origin/$branch" "$commit" 2>/dev/null) && whole "$base"; then
		echo "$base"
		exit 0
	fi
	git fetch -q --no-tags --deepen=100 origin "${want[@]}"
done
echo "no merge base of $branch and ${commit:0:12} within 3000 commits" >&2
exit 1
