#!/bin/bash
# lib.sh: shared paths and helpers, sourced by every script that runs inside a Modal container.
# Paths never change: ninja and ccache remember absolute paths, so a restored snapshot must land at the same place.
set -euo pipefail

CN=/opt/cn                      # config.env, args.gn, container/, patches/ (added by modalapp/build.py)
VOL=/vol                        # the Modal Volume (persistent)
W=/work                         # local SSD of this container (gone when the function ends)
REPO=$W/c151                    # git checkout of webOS-ports/chromium151 (repo root)
SRC=$REPO/src                   # Chromium "src"
OUT=$SRC/out/tv                 # build directory
SYSROOT=$W/sysroot-tv           # target sysroot (args.gn target_sysroot)
DEPOT=$W/depot_tools
SNAP=$VOL/snap                  # snapshots: <name>.tar.zst + <name>.json
LOGS=$VOL/logs                  # <build-id>.log, <build-id>.json (status), <build-id>.errors.txt
ART=$VOL/artifacts              # <build-id>/... outputs
TVD=$VOL/tv                     # TV files (LG proprietary): rootfs.sqfs, apps.tar + .sha256
mkdir -p "$W" "$SNAP" "$LOGS" "$ART"

set -a; . "$CN/config.env"; set +a
# Build variants (2026-11-01). VARIANT=compat in config.env: the browser for older TVs (webOS 4.4+, glibc 2.24),
# linked against a glibc 2.24 sysroot. Its own sysroot, out dir and ccache snapshots, so the main build is untouched.
VARIANT=${VARIANT:-main}
if [ "$VARIANT" = compat ]; then
	SYSROOT_SNAP=sysroot-compat CCACHE_SNAP=ccache-compat OUT_SUFFIX=-compat
else
	SYSROOT_SNAP=sysroot CCACHE_SNAP=ccache OUT_SUFFIX=
fi
BUILD_ID=${BUILD_ID:-$(date -u +%Y%m%dT%H%M%SZ)-$(basename "$0" .sh)}
LOG=$W/run-$BUILD_ID.log
STATUS=$LOGS/$BUILD_ID.json
META="python3 $CN/container/meta.py"

LLVM=$SRC/third_party/llvm-build/Release+Asserts/bin
# /usr/local/bin first: it has the real ninja; depot_tools (last) has a ninja wrapper that needs gclient
export PATH=/usr/local/bin:$PATH:$DEPOT
export RUST_TARGET_PATH=$REPO/rust-targets

# ccache: the cache directory lives on local SSD during the run and is saved as snapshot "ccache".
export CCACHE_DIR=$W/ccache CCACHE_BASEDIR=$SRC CCACHE_NOHASHDIR=1 CCACHE_COMPILERCHECK=content \
	CCACHE_SLOPPINESS=include_file_ctime,include_file_mtime,time_macros,file_macro,pch_defines \
	CCACHE_MAXSIZE=$CCACHE_MAXSIZE CCACHE_COMPRESS=1 CCACHE_DEPEND=1

log() { echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$LOG" >&2; }
push_log() { cp -f "$LOG" "$LOGS/$BUILD_ID.log" 2>/dev/null || true; sync "$LOGS" 2>/dev/null || true; }
st() { $META set "$STATUS" "$@"; }                     # st key=value ...  (status file of this run)
die() { log "ERROR: $*"; st status=failed error="$*"; push_log; exit 1; }
trap 'rc=$?; [ $rc -ne 0 ] && { st status=failed exit_code=$rc 2>/dev/null; push_log; }; exit $rc' EXIT

st status=running script="$(basename "$0")" started="$(date -u +%FT%TZ)" host_cpus="$(nproc)"

# snap_restore NAME DEST_PARENT: unpack snapshot NAME into DEST_PARENT. Returns 1 if there is no such snapshot.
snap_restore() {
	local f=$SNAP/$1.tar.zst t0=$SECONDS
	[ -f "$f" ] || { log "no snapshot '$1'"; return 1; }
	log "restore snapshot $1 ($(du -h "$f" | cut -f1)) into $2"
	mkdir -p "$2"
	zstd -dc -T0 -q "$f" | tar -C "$2" -xf -
	log "restored $1 in $((SECONDS - t0)) s"
}

# snap_save NAME PARENT PATH... [-- TAR_OPTIONS...]: pack PARENT/PATH... as snapshot NAME (atomic replace).
# --format=posix keeps sub-second timestamps, so ninja sees exactly the same file times after a restore.
snap_save() {
	local name=$1 parent=$2; shift 2
	local tmp=$SNAP/.$name.tmp.tar.zst t0=$SECONDS
	log "save snapshot $name"
	tar --format=posix -C "$parent" "${SNAP_TAR_OPTS[@]}" -cf - "$@" | zstd -T0 -3 -q -f -o "$tmp"
	mv -f "$tmp" "$SNAP/$name.tar.zst"
	sync "$SNAP" 2>/dev/null || true
	$META set "$SNAP/$name.json" saved="$(date -u +%FT%TZ)" bytes="$(stat -c %s "$SNAP/$name.tar.zst")" build_id="$BUILD_ID"
	log "saved $name ($(du -h "$SNAP/$name.tar.zst" | cut -f1)) in $((SECONDS - t0)) s"
}
SNAP_TAR_OPTS=()

need_src() { snap_restore src "$W" || die "no source snapshot: run setup_source first"; }
need_sysroot() { snap_restore $SYSROOT_SNAP "$W" || die "no $SYSROOT_SNAP snapshot: run build_sysroot (or build_sysroot_compat) first"; }
# softfp compiler-rt builtins, at the path Chromium links (build/config/clang/BUILD.gn compiler_builtins)
need_builtins() {
	snap_restore builtins "$W" || die "no builtins snapshot: run build_builtins first"
	local d; d=$LLVM/../lib/clang/$(ls "$LLVM/../lib/clang")/lib/armv7-unknown-linux-gnueabihf
	mkdir -p "$d"; cp -p "$W/builtins/libclang_rt.builtins.a" "$d/"
	BUILTINS=$d/libclang_rt.builtins.a
}
