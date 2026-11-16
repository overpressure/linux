#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Which headsets' kernels a run builds (.github/workflows/build.yml):
#
#   headsets.sh [--self] REF
#
# REF is a branch or tag of this repository. When its first part names a headset (a repository
# <owner>/<headset> with kernel/build-tree.sh), that headset is built from the run's own commit.
# Otherwise REF is a layer: every headset's branch built on it (.github/layers, followed upward) is
# built with the run's commit merged in. --self refuses a layer: a push builds only a headset's own
# branch or tag. Prints a JSON list of {device, branch, merge, recipe}, recipe being the commit of the
# headset repository's default branch, so that every build of the run uses the same.
set -euo pipefail
self=false
if [ "${1:-}" = --self ]; then
	self=true
	shift
fi
ref=${1:?usage: headsets.sh [--self] REF}
owner=${GITHUB_REPOSITORY_OWNER:-overpressure}
layers=$(cd "$(dirname "$0")/.." && pwd)/layers

# The commit of <owner>/DEVICE's default branch when it is a headset's repository, else nothing.
recipe() {
	gh api "repos/$owner/$1/contents/kernel/build-tree.sh" --silent 2>/dev/null || return 0
	gh api "repos/$owner/$1/commits/$(gh api "repos/$owner/$1" --jq .default_branch)" --jq .sha
}
entry() {
	jq -cn --arg device "$1" --arg branch "$2" --argjson merge "$3" --arg recipe "$4" '$ARGS.named'
}

sha=$(recipe "${ref%%/*}")
if [ -n "$sha" ]; then
	entry "${ref%%/*}" "$ref" false "$sha" | jq -cs .
	exit 0
fi
if $self; then
	echo "$ref is a layer without a headset: a push to it builds nothing (build.yml's push branches leave it out)" >&2
	exit 1
fi
# Up from the layer, each branch once.
queue=("$ref")
seen=" $ref "
found=()
while [ ${#queue[@]} -gt 0 ]; do
	from=${queue[0]}
	queue=("${queue[@]:1}")
	while read -r source into; do
		[ "$source" = "$from" ] || continue
		case $seen in *" $into "*) continue ;; esac
		seen="$seen$into "
		queue+=("$into")
		sha=$(recipe "${into%%/*}")
		[ -z "$sha" ] || found+=("$(entry "${into%%/*}" "$into" true "$sha")")
	done < <(grep -v '^#' "$layers")
done
if [ ${#found[@]} -eq 0 ]; then
	echo "no headset's branch is built on $ref (.github/layers): nothing to build" >&2
	exit 1
fi
printf '%s\n' "${found[@]}" | jq -cs .
