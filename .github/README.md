# overpressure/linux

**TL;DR:** Valve's Steam Frame kernel (`linux-valve` 7.2) with the
[overpressure](https://github.com/overpressure/overpressure) project's changes for other standalone VR headsets, one
branch per layer. Changes move up the layers by merge pull requests, never by rebase. A headset's release is an
annotated tag `<headset>/7.2-N`, built twice by CI and released only when both builds agree; the headset's repository
pins that tag by commit and file hashes.

## Branches

| Branch | What |
| --- | --- |
| `valve/7.2` | Valve's own history: the public `sm8650-7.2` branch through the [evlaV mirror](https://github.com/evlaV/deckard-linux-no-oopsies), moved only forward, untouched |
| `common/7.2` | fixes any headset on Valve's tree can meet, and this repository's CI (`.github/`) |
| `sm8250/7.2` | the Snapdragon XR2's (SM8250) blocks |
| `phoenix/7.2` | the PICO 4 ([overpressure/phoenix](https://github.com/overpressure/phoenix)): its drivers, device tree, `arch/arm64/configs/pico4.config` and driver tests; the default branch |

[`.github/layers`](layers) lists each branch and the branch it takes its changes from. A headset's branch is named
after its repository, `overpressure/<headset>`, whose `kernel/build-tree.sh` builds it; the PICO 4's layers are
described in [phoenix's docs/kernel.md](https://github.com/overpressure/phoenix/blob/main/docs/kernel.md#changes-to-valves-tree).

A change lands on the lowest layer it belongs to and moves up by pull request, merged with a merge commit; nothing is
rebased or force-pushed. A conflict is resolved in the pull request that meets it. Rulesets let the layers change only
by such pull requests with a passing `ci` check, `valve/7.2` only move forward, and tags never move.

## Workflows

| Workflow | When | What |
| --- | --- | --- |
| [Valve](workflows/valve.yml) | Mondays 05:23 UTC, and by hand | moves `valve/7.2` forward to Valve's newest commit ([scripts/valve.sh](scripts/valve.sh)) and starts Layers for it; a rewritten Valve branch fails the run. A schedule runs only from the default branch's copy |
| [Layers](workflows/layers.yml) | a push to a layer, and by hand | opens a pull request from the branch into every layer built on it that lacks its commits, unless one is open, and starts its Build ([scripts/layers.sh](scripts/layers.sh)) |
| [Build](workflows/build.yml) | pull requests, pushes to a headset's branch, tags, and the Layers workflow's runs for its pull requests | builds each headset's kernel; for a pull request into a layer without a headset, every headset's branch built on it with the pull request merged in; lints the scripts and workflows; releases tags. Its `ci` job is the required check |

Each week, after the Valve workflow: review and merge its pull request into `common/7.2`, then the pull requests that
follow up the layers, each once its `ci` passes.

## Tags and releases

| Tag | What |
| --- | --- |
| `<headset>/7.2-N` | a headset's release, `N` counting up from 1 (`phoenix/7.2-1`) |
| `valve/7.2-<hash>` | the commit of Valve's `linux-valve` that one of Valve's source packages for the Steam Frame was built from |

To release a headset's kernel:

1. An annotated tag on the headset's branch, its message the title and what changed:
   `git tag -a phoenix/7.2-2 origin/phoenix/7.2`, then push it.
2. CI builds the tag twice on two machines and releases it only when both give the same files: `Image.gz`, the
   device tree, `config`, `version`, `build.json` (the commit, the kernel's banner, the tooling's commits, the build
   image) and `SHA256SUMS`. The notes name the tooling's commits, the change list since the headset's previous tag
   and Valve's commit. No release is marked the latest: several headsets share this repository.
3. The headset's repository pins the tag, its commit and the files' hashes (phoenix's
   [kernel/release.json](https://github.com/overpressure/phoenix/blob/main/kernel/release.json)) and checks the
   files against them.

A release is an exact pin, not a channel: there are no preview or beta kernels here. Before a headset pins a new
tag, its own release checklist verifies it on the hardware (phoenix's
[docs/release-checklist.md](https://github.com/overpressure/phoenix/blob/main/docs/release-checklist.md)).

## The layers' pull requests

Layers opens its pull requests with the workflow's own token, and a pull request opened with it starts no workflow.
So Layers starts each one's Build itself: a run on the pull request's base branch with `head` set to the branch
that moves up (`valve/7.2` among them, whose tree has no workflows). It builds every headset's branch on that base
with `head` merged in, and sets the status `ci` on `head`'s commit, the check the pull request waits for. Its own
gate job is named `verdict for <head>`: that run belongs to the base's commit, which another pull request may have
as its head.

Each content is built once. A check of a change is keyed by the kernel's sources (the tree without `.github/`, so a
change to the CI alone builds nothing new), so the layers' pull requests find the build of the pull request that
brought the change and take seconds; a tag builds again, twice and keyed by its commit, which `uname -v` and the
release name. One change costs one build in its pull request, one through a warm ccache on the push after its merge,
and two in its headset's tag. A check build compiles through ccache, whose objects the cache of its
branch, its base or the default branch carries over, so it recompiles only what the change touches. GitHub keeps a
run's cache for its own branch, and only the default branch's reaches every pull request: so a push to a headset's
branch compiles through ccache instead of taking the pull request's build, and keeps that cache current; a tag's two
builds start clean (a change to the kernel's configuration touches every file, so it compiles everything anyway).
