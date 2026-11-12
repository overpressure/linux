#!/usr/bin/env bash
# SPDX-License-Identifier: Unlicense
# The PICO 4's driver tests that run on the host, against stub kernel headers (stubs/): the battery
# driver on a fake PM8150B. The kernel's CI runs them on every phoenix branch and tag.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
echo ">> test_pico4_battery"
"${CC:-cc}" -O1 -Wall -Wextra -Werror -std=gnu11 -I"$here/stubs" -o "$work/test_pico4_battery" \
	"$here/test_pico4_battery.c"
"$work/test_pico4_battery"
