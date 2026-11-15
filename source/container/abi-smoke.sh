#!/bin/bash
# abi-smoke.sh: build smoke/softfp_test.c for the TV exactly the way Chromium compiles and links
# (build/config/compiler_cpu_abi.gn: --target=arm-linux-gnueabihf + -mfloat-abi=softfp; compiler-rt builtins on the
# link line; patch 0001: TV loader), plus the host reference output.
# artifacts/abi-smoke/: softfp_test.chromium (with compiler-rt builtins, = what Chromium does), softfp_test.libgcc
# (without), expected.txt, elf-report.txt, abi-report.txt. The ARM binaries are then run on a TV.
. /opt/cn/container/lib.sh
need_src
need_sysroot
need_builtins
O=$ART/abi-smoke; rm -rf "$O"; mkdir -p "$O"
C=$CN/container/smoke/softfp_test.c
TGT=(--target=arm-linux-gnueabihf -march=armv7-a -mfloat-abi=softfp -mfpu=neon -mthumb --sysroot="$SYSROOT"
	-fuse-ld=lld -O2 -Wl,-dynamic-linker,"$TV_LOADER")

HOST_SYSROOT=$(ls -d "$SRC"/build/linux/debian_*_amd64-sysroot | head -1)
"$LLVM/clang" --sysroot="$HOST_SYSROOT" -fuse-ld=lld -O2 "$C" -o "$W/host_test" -lm
"$W/host_test" > "$O/expected.txt"

"$LLVM/clang" "${TGT[@]}" "$C" -o "$O/softfp_test.chromium" "$BUILTINS" -lm 2>&1 | tee -a "$LOG" || die "chromium-style link failed"
"$LLVM/clang" "${TGT[@]}" "$C" -o "$O/softfp_test.libgcc" -lm 2>&1 | tee -a "$LOG" || die "libgcc link failed"

{
	echo "== compiler-rt builtins: $BUILTINS"
	"$LLVM/llvm-readelf" -A "$BUILTINS" | grep -m1 'Tag_ABI_VFP_args' || echo "   no Tag_ABI_VFP_args (soft-float calling convention: OK)"
	for b in "$O"/softfp_test.*; do
		echo "== $(basename "$b")"
		"$LLVM/llvm-readelf" -h "$b" | grep Flags
		"$LLVM/llvm-readelf" -l "$b" | grep -o 'interpreter: [^]]*' || echo "   no interpreter"
		"$LLVM/llvm-readelf" -A "$b" | grep -E 'Tag_ABI_VFP_args|Tag_FP_arch' || true
		"$LLVM/llvm-readelf" -d "$b" | grep NEEDED || true
	done
} | tee "$O/elf-report.txt" | tee -a "$LOG"

mkdir -p "$W/abi" && cp "$O"/softfp_test.* "$W/abi/"
set +e
LLVM=$LLVM SYSROOT=$SYSROOT TV_GLIBC_MAX=$TV_GLIBC_MAX TV_LOADER=$TV_LOADER python3 "$CN/container/check-abi.py" "$W/abi" > "$O/abi-report.txt" 2>&1
abi=$?
set -e
cat "$O/abi-report.txt" | tee -a "$LOG"
ls -l "$O" | tee -a "$LOG"
st status=done abi_ok=$((abi == 0))
push_log
[ $abi -eq 0 ] || die "check-abi found problems (artifacts/abi-smoke/abi-report.txt)"
