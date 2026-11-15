#!/bin/bash
# setup-source.sh: chromium151 at C151_COMMIT + Chromium's own toolchains, saved as snapshot "src".
# Safe to run again: it restores the old snapshot first and only moves it to the new commit, so git rewrites only the
# files that changed. Unchanged files keep their timestamps and ninja/ccache reuse their old results.
. /opt/cn/container/lib.sh

if snap_restore src "$W"; then
	have=$(git -C "$REPO" rev-parse HEAD)
	log "snapshot is at $have"
else
	have=none
	log "first run: fresh clone"
	git init -q "$REPO"
	git -C "$REPO" remote add origin "$C151_REPO"
fi

if [ "$have" != "$C151_COMMIT" ]; then
	log "fetch $C151_COMMIT (shallow)"
	git -C "$REPO" fetch -q --depth 1 origin "$C151_COMMIT"
	git -C "$REPO" checkout -q --detach FETCH_HEAD
	git -C "$REPO" reset -q --hard FETCH_HEAD     # snapshots are always clean upstream; patches come later
fi
[ "$(git -C "$REPO" rev-parse HEAD)" = "$C151_COMMIT" ] || die "checkout is not at $C151_COMMIT"
log "files: $(git -C "$REPO" ls-files | wc -l)"

# depot_tools: only for download_from_google_storage.py-style helpers some hooks use (no gclient sync).
[ -d "$DEPOT/.git" ] || git clone -q --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git "$DEPOT"
export DEPOT_TOOLS_UPDATE=0

cd "$SRC"
log "clang (Google prebuilt for this revision)"
python3 tools/clang/scripts/update.py
log "rust (Google prebuilt for this revision)"
python3 tools/rust/update_rust.py
log "host sysroot (amd64) for host tools"
python3 build/linux/sysroot_scripts/install-sysroot.py --arch=amd64

NODE_DIR=third_party/node/linux/node-linux-x64
if ! "$NODE_DIR/bin/node" --version 2>/dev/null | grep -qx "$NODE_VERSION"; then
	log "node $NODE_VERSION"
	rm -rf "$NODE_DIR"; mkdir -p "$NODE_DIR"
	curl -fsSL "https://nodejs.org/dist/$NODE_VERSION/node-$NODE_VERSION-linux-x64.tar.xz" | tar -xJ -C "$NODE_DIR" --strip-components=1
fi

# Generated files that gclient hooks normally write (DEPS "hooks": lastchange*, gpu_lists_version).
log "lastchange hooks"
python3 build/util/lastchange.py -o build/util/LASTCHANGE
python3 build/util/lastchange.py -m CHROMIUM --commit-position-header build/util/LASTCHANGE_commit_position.h || true
python3 build/util/lastchange.py -m GPU_LISTS_VERSION --revision-id-only --header gpu/config/gpu_lists_version.h
python3 build/util/lastchange.py -m SKIA_COMMIT_HASH -s third_party/skia --header skia/ext/skia_commit_hash.h
python3 build/util/lastchange.py -m DAWN_COMMIT_HASH -s third_party/dawn --revision gpu/webgpu/DAWN_VERSION \
	--header gpu/webgpu/dawn_commit_hash.h || true

# Rust target specs (the go/no-go needed RUST_TARGET_PATH for the Rust std build).
RUSTC=$SRC/third_party/rust-toolchain/bin/rustc
mkdir -p "$RUST_TARGET_PATH"
for t in x86_64-unknown-linux-gnu armv7-unknown-linux-gnueabi; do
	RUSTC_BOOTSTRAP=1 "$RUSTC" -Z unstable-options --print target-spec-json --target "$t" > "$RUST_TARGET_PATH/$t.json"
done

clang_v=$("$LLVM/clang" --version | head -1)
gn_v=$(buildtools/linux64/gn --version)
rust_v=$("$RUSTC" --version)
log "clang: $clang_v | gn: $gn_v | rustc: $rust_v | node: $("$NODE_DIR/bin/node" --version) | ninja: $(ninja --version)"

SNAP_TAR_OPTS=(--exclude=c151/src/out)
snap_save src "$W" c151 depot_tools
$META set "$SNAP/src.json" commit="$C151_COMMIT" clang="$clang_v" gn="$gn_v" rustc="$rust_v" node="$NODE_VERSION"
st status=done commit="$C151_COMMIT" clang="$clang_v" src_snapshot_bytes="$(stat -c %s "$SNAP/src.tar.zst")"
push_log
