#!/bin/sh
# check-patches.sh PATCH_DIR REPO COMMIT: check that every *.patch in PATCH_DIR applies, in name order, to COMMIT.
#
# why: a patch that does not apply only shows up after a long Modal build. This checks the series first, against the
# pinned source tree. It reads COMMIT into a temporary index and runs "git apply --cached" on each patch, so the
# work tree of REPO is never touched. Patches are applied one after another, so each one sees the result of the ones
# before it. Files whose name ends in .off are not part of the series.
#
# Usage: sh container/check-patches.sh PATCH_DIR REPO COMMIT
# Prints "ok NAME" per patch and exits 0. On the first patch that fails it prints "FAIL NAME" with git's message and
# exits 1.
set -eu

[ $# -eq 3 ] || { echo "usage: $0 PATCH_DIR REPO COMMIT" >&2; exit 2; }
PD=$(cd "$1" && pwd)
R=$(cd "$2" && pwd)
C=$3

I=$(mktemp "${TMPDIR:-/tmp}/check-patches.XXXXXX")
E=$I.err
trap 'rm -f "$I" "$E"' EXIT

list=$(ls -1 "$PD" | grep '\.patch$' | LC_ALL=C sort || true)
[ -n "$list" ] || { echo "no *.patch files in $PD" >&2; exit 2; }

GIT_INDEX_FILE=$I git -C "$R" -c gc.auto=0 read-tree "$C"

n=0
for name in $list; do
	if GIT_INDEX_FILE=$I git -C "$R" apply --cached "$PD/$name" 2>"$E"; then
		echo "ok $name"
		n=$((n + 1))
	else
		echo "FAIL $name"
		sed 's/^/  /' "$E"
		exit 1
	fi
done
echo "$n patches apply to $C"
