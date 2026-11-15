#!/bin/bash
# build-builtins.sh: compiler-rt builtins for ARMv7 softfp, saved as snapshot "builtins".
# Why: Chromium links $clang/lib/clang/<v>/lib/armv7-unknown-linux-gnueabihf/libclang_rt.builtins.a explicitly
# (build/config/clang/BUILD.gn "compiler_builtins"). Google's clang package has none for 32-bit ARM, and a
# hard-float one would break the TV's softfp ABI. We build it from the LLVM commit Chromium's clang was made from.
# need_builtins (lib.sh) puts it in place before every build.
. /opt/cn/container/lib.sh
need_src
need_sysroot
REV=$(sed -n "s/^CLANG_REVISION = '\(.*\)'/\1/p" "$SRC/tools/clang/scripts/update.py")   # llvmorg-23-init-NNN-g<sha>
SHORT=${REV##*-g}
FULL=$(curl -fsSL "https://api.github.com/repos/llvm/llvm-project/commits/$SHORT" | python3 -c 'import json,sys; print(json.load(sys.stdin)["sha"])')
log "LLVM $REV = $FULL"
L=$W/llvm-project
git init -q "$L"
git -C "$L" remote add origin https://github.com/llvm/llvm-project.git
git -C "$L" sparse-checkout set compiler-rt cmake llvm/cmake runtimes third-party
git -C "$L" fetch -q --depth 1 --filter=blob:none origin "$FULL"
git -C "$L" checkout -q FETCH_HEAD
CV=$(ls "$LLVM/../lib/clang")
# same triple as Chromium (sysroot headers are under the gnueabihf multiarch dir); -mfloat-abi=softfp sets the ABI
FL="-march=armv7-a -mfloat-abi=softfp -mfpu=neon -O2"
B=$W/builtins-build
# Chromium's clang package has llvm-ar but not always llvm-ranlib; llvm-ar acts as ranlib under that name
TB=$W/llvm-tools; mkdir -p "$TB"
ls "$LLVM" | tr '\n' ' ' | tee -a "$LOG"; echo | tee -a "$LOG"
RANLIB=$LLVM/llvm-ranlib; [ -x "$RANLIB" ] || { ln -sf "$LLVM/llvm-ar" "$TB/llvm-ranlib"; RANLIB=$TB/llvm-ranlib; }
NM=$LLVM/llvm-nm; [ -x "$NM" ] || NM=$(command -v nm)
cmake -G Ninja -S "$L/compiler-rt/lib/builtins" -B "$B" \
	-DCMAKE_BUILD_TYPE=Release -DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=arm \
	-DCMAKE_C_COMPILER="$LLVM/clang" -DCMAKE_CXX_COMPILER="$LLVM/clang++" -DCMAKE_ASM_COMPILER="$LLVM/clang" \
	-DCMAKE_AR="$LLVM/llvm-ar" -DCMAKE_RANLIB="$RANLIB" -DCMAKE_NM="$NM" \
	-DCMAKE_C_COMPILER_TARGET=arm-linux-gnueabihf -DCMAKE_ASM_COMPILER_TARGET=arm-linux-gnueabihf \
	-DCMAKE_C_FLAGS="$FL" -DCMAKE_ASM_FLAGS="$FL" -DCMAKE_SYSROOT="$SYSROOT" \
	-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
	-DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF -DCOMPILER_RT_BUILD_BUILTINS=ON > "$W/cmake.txt" 2>&1 ||
	{ grep -A40 'CMake Error' "$W/cmake.txt" | head -60 | tee -a "$LOG"; die "cmake failed"; }
tail -3 "$W/cmake.txt" | tee -a "$LOG"
ninja -C "$B" -k 0 > "$W/ninja.txt" 2>&1 || { grep -B3 -A6 "error:" "$W/ninja.txt" | head -80 | tee -a "$LOG"; die "builtins compile failed"; }
tail -2 "$W/ninja.txt" | tee -a "$LOG"
A=$(find "$B" -name 'libclang_rt.builtins*.a' | head -1)
[ -f "$A" ] || die "builtins archive not built"
if "$LLVM/llvm-readelf" -A "$A" | grep -q 'Tag_ABI_VFP_args: VFP registers'; then die "builtins are hard-float"; fi
O=$W/builtins; rm -rf "$O"; mkdir -p "$O"
cp "$A" "$O/libclang_rt.builtins.a"
log "builtins: $(basename "$A"), $(stat -c %s "$A") bytes, $("$NM" "$A" 2>/dev/null | grep -c ' T ') functions, clang $CV"
snap_save builtins "$W" builtins
$META set "$SNAP/builtins.json" llvm="$FULL" clang_version="$CV"
st status=done llvm="$FULL"
push_log
