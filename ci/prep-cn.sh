#!/bin/bash
# prep-cn.sh VARIANT: copy the checked-out source/ (in ./wc) to /home/runner/cnci/cn, the folder mounted as /opt/cn.
# Only args.gn changes, and only in this copy: cc_wrapper = "" (no ccache; the build folder itself carries the state
# from one job to the next). VARIANT is "default" (the one build).
set -euo pipefail
V=${1:?variant}
B=/home/runner/cnci
rm -rf "$B/cn"; cp -a "$GITHUB_WORKSPACE/wc/source" "$B/cn"
A=$B/cn/args.gn
grep -q '^cc_wrapper = "ccache"' "$A" || { echo "args.gn: no cc_wrapper line"; exit 1; }
sed -i 's/^cc_wrapper = "ccache".*/cc_wrapper = ""   # CI: no ccache (the build folder carries the state between jobs)/' "$A"
case "$V" in
default) ;;
*) echo "unknown variant $V"; exit 1 ;;
esac
echo "args.gn changes for variant $V:"
diff "$GITHUB_WORKSPACE/wc/source/args.gn" "$A" || true
